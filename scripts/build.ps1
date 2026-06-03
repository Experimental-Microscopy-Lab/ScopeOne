$ErrorActionPreference = "Stop"

$target = "all"
$coreConfigureOption = @()
$guiConfigureOption = @()
$coreBuildOption = @()
$guiBuildOption = @()
$configure = $false
$clean = $false
$run = $false
$package = $false
$skipCoreInstall = $false

function Read-OptionValue {
    param(
        [string[]]$Arguments,
        [ref]$Index,
        [string]$OptionName
    )

    $nextIndex = $Index.Value + 1
    if ($nextIndex -ge $Arguments.Count) {
        throw "Missing value for option $OptionName."
    }

    $Index.Value = $nextIndex
    return $Arguments[$nextIndex]
}

for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = $args[$i]
    switch ($arg) {
        { $_ -in @("--target", "-target") } {
            $target = Read-OptionValue -Arguments $args -Index ([ref]$i) -OptionName $arg
            continue
        }
        { $_ -in @("--coreConfigureOption", "-coreConfigureOption") } {
            $coreConfigureOption += Read-OptionValue -Arguments $args -Index ([ref]$i) -OptionName $arg
            continue
        }
        { $_ -in @("--guiConfigureOption", "-guiConfigureOption") } {
            $guiConfigureOption += Read-OptionValue -Arguments $args -Index ([ref]$i) -OptionName $arg
            continue
        }
        { $_ -in @("--coreBuildOption", "-coreBuildOption") } {
            $coreBuildOption += Read-OptionValue -Arguments $args -Index ([ref]$i) -OptionName $arg
            continue
        }
        { $_ -in @("--guiBuildOption", "-guiBuildOption") } {
            $guiBuildOption += Read-OptionValue -Arguments $args -Index ([ref]$i) -OptionName $arg
            continue
        }
        { $_ -in @("--configure", "-configure") } {
            $configure = $true
            continue
        }
        { $_ -in @("--clean", "-clean") } {
            $clean = $true
            continue
        }
        { $_ -in @("--run", "-run") } {
            $run = $true
            continue
        }
        { $_ -in @("--package", "-package") } {
            $package = $true
            continue
        }
        { $_ -in @("--skipCoreInstall", "-skipCoreInstall") } {
            $skipCoreInstall = $true
            continue
        }
        default {
            throw "Unknown option: $arg"
        }
    }
}

if ($target -notin @("all", "core", "gui")) {
    throw "Invalid target '$target'. Expected one of: all, core, gui."
}
if ($package -and $target -eq "core") {
    throw "--package requires --target gui or --target all."
}

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-Step {
    param(
        [string]$Label,
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )

    Write-Step $Label
    Write-Host "$FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray

    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$Label failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }
}

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Normalize-CMakePath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    return ($Path -replace "\\", "/").TrimEnd("/")
}

function Get-CMakeCacheValue {
    param(
        [string]$CachePath,
        [string]$Key
    )

    if (-not (Test-Path $CachePath)) {
        return $null
    }

    $match = Select-String -Path $CachePath -Pattern "^${Key}(:[^=]+)?=" | Select-Object -First 1
    if (-not $match) {
        return $null
    }

    $parts = $match.Line -split "=", 2
    if ($parts.Count -ne 2) {
        return $null
    }

    return $parts[1]
}

function Find-ConfigureOverride {
    param(
        [string[]]$Options,
        [string]$Prefix
    )

    return $Options | Where-Object { $_ -like "$Prefix*" } | Select-Object -First 1
}

$cmake = (Get-Command cmake -ErrorAction Stop).Source

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

$guiSourceDir = $repoRoot
$guiBuildDir = Join-Path $repoRoot "build"

$coreSourceDir = Join-Path $repoRoot "ScopeOneCore"
$coreBuildDir = Join-Path $coreSourceDir "build"
$coreInstallDir = Join-Path $coreSourceDir "install"
$config = "Release"
$coreCachePath = Join-Path $coreBuildDir "CMakeCache.txt"
$guiCachePath = Join-Path $guiBuildDir "CMakeCache.txt"

if ($clean) {
    if ($target -in @("all", "gui") -and (Test-Path $guiBuildDir)) {
        Write-Step "Removing GUI build directory"
        Remove-Item -LiteralPath $guiBuildDir -Recurse -Force
    }
    if ($target -in @("all", "core") -and (Test-Path $coreBuildDir)) {
        Write-Step "Removing ScopeOneCore build directory"
        Remove-Item -LiteralPath $coreBuildDir -Recurse -Force
    }
}

Ensure-Directory $repoRoot

$coreConfigureOptionOverride = $coreConfigureOption.Count -gt 0
$guiConfigureOptionOverride = $guiConfigureOption.Count -gt 0

$needCoreConfigure = $configure -or $coreConfigureOptionOverride -or -not (Test-Path $coreCachePath)
$needGuiConfigure = $configure -or $guiConfigureOptionOverride -or -not (Test-Path $guiCachePath)

$installPrefixOverride = Find-ConfigureOverride -Options $coreConfigureOption -Prefix "-DCMAKE_INSTALL_PREFIX="

if (-not $needCoreConfigure -and $target -in @("all", "core")) {
    $cachedInstallPrefix = Normalize-CMakePath (Get-CMakeCacheValue -CachePath $coreCachePath -Key "CMAKE_INSTALL_PREFIX")
    $expectedInstallPrefix = Normalize-CMakePath $coreInstallDir

    if (-not $installPrefixOverride -and $cachedInstallPrefix -ne $expectedInstallPrefix) {
        $needCoreConfigure = $true
    }
}

if ($target -in @("all", "core")) {
    if ($needCoreConfigure) {
        $coreConfigureArgs = @(
            "-S", $coreSourceDir,
            "-B", $coreBuildDir
        )

        if (-not $installPrefixOverride) {
            $coreConfigureArgs += "-DCMAKE_INSTALL_PREFIX=$coreInstallDir"
        }

        $coreConfigureArgs += $coreConfigureOption

        Invoke-Step `
            -Label "Configuring ScopeOneCore" `
            -FilePath $cmake `
            -Arguments $coreConfigureArgs `
            -WorkingDirectory $repoRoot
    }

    $coreBuildArgs = @(
        "--build", $coreBuildDir,
        "--config", $config,
        "--parallel"
    ) + $coreBuildOption

    Invoke-Step `
        -Label "Building ScopeOneCore ($config)" `
        -FilePath $cmake `
        -Arguments $coreBuildArgs `
        -WorkingDirectory $repoRoot

    if (-not $skipCoreInstall) {
        Invoke-Step `
            -Label "Installing ScopeOneCore into local prefix" `
            -FilePath $cmake `
            -Arguments @(
                "--build", $coreBuildDir,
                "--config", $config,
                "--target", "INSTALL"
            ) `
            -WorkingDirectory $repoRoot
    }
}

if ($target -in @("all", "gui")) {
    if ($needGuiConfigure) {
        $guiConfigureArgs = @(
            "-S", $guiSourceDir,
            "-B", $guiBuildDir,
            "-DScopeOneCore_ROOT=$coreInstallDir"
        ) + $guiConfigureOption

        Invoke-Step `
            -Label "Configuring ScopeOne GUI" `
            -FilePath $cmake `
            -Arguments $guiConfigureArgs `
            -WorkingDirectory $repoRoot
    }

    $guiBuildArgs = @(
        "--build", $guiBuildDir,
        "--config", $config,
        "--parallel"
    ) + $guiBuildOption

    Invoke-Step `
        -Label "Building ScopeOne GUI ($config)" `
        -FilePath $cmake `
        -Arguments $guiBuildArgs `
        -WorkingDirectory $repoRoot

    if ($package) {
        Invoke-Step `
            -Label "Packaging ScopeOne ($config)" `
            -FilePath $cmake `
            -Arguments @(
                "--build", $guiBuildDir,
                "--config", $config,
                "--parallel",
                "--target", "PACKAGE"
            ) `
            -WorkingDirectory $repoRoot
    }
}

$guiExe = Join-Path $guiBuildDir "$config\ScopeOne.exe"
$packageCandidates = Get-ChildItem -LiteralPath $guiBuildDir -Filter "ScopeOne-*-win-x64.zip" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending
$latestPackage = if ($packageCandidates) { $packageCandidates[0].FullName } else { $null }

Write-Step "Summary"
Write-Host "Target: $target"
Write-Host "Config: $config"
Write-Host "ScopeOneCore install: $coreInstallDir"
if (Test-Path $guiExe) {
    Write-Host "GUI executable: $guiExe"
}
if ($latestPackage) {
    Write-Host "Latest package: $latestPackage"
}

if ($run) {
    if (-not (Test-Path $guiExe)) {
        throw "Cannot run ScopeOne because the executable was not found at $guiExe."
    }

    Invoke-Step `
        -Label "Launching ScopeOne" `
        -FilePath $guiExe `
        -Arguments @() `
        -WorkingDirectory (Split-Path -Parent $guiExe)
}
