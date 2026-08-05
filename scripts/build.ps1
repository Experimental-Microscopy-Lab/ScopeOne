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

if ($target -notin @("all", "core", "gui", "scopewriter")) {
    throw "Invalid target '$target'. Expected one of: all, core, gui, scopewriter."
}
if ($package -and $target -in @("core", "scopewriter")) {
    throw "--package requires --target gui or --target all."
}
if ($run -and $target -eq "scopewriter") {
    throw "--run is not available for the scopewriter target."
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

function Import-MsvcEnvironment {
    if ($env:OS -ne "Windows_NT") {
        return
    }

    if ($env:VSCMD_VER -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $env:CC = "cl.exe"
        $env:CXX = "cl.exe"
        return
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        throw "Visual Studio Installer was not found."
    }
    $installationPath = & $vsWhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
        throw "A Visual Studio installation with the C++ toolchain was not found."
    }

    $devShellModule = Join-Path $installationPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path $devShellModule)) {
        throw "Visual Studio developer PowerShell module was not found."
    }
    try {
        Import-Module $devShellModule -ErrorAction Stop
        Enter-VsDevShell `
            -VsInstallPath $installationPath `
            -SkipAutomaticLocation `
            -Arch amd64 `
            -HostArch amd64 `
            -DevCmdArguments "-no_logo" `
            -ErrorAction Stop | Out-Null
        $msvcPath = $env:Path
        Remove-Item Env:PATH -ErrorAction SilentlyContinue
        Remove-Item Env:Path -ErrorAction SilentlyContinue
        $env:Path = $msvcPath
    }
    catch {
        throw "Failed to initialize the Visual Studio developer environment: $($_.Exception.Message)"
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Visual Studio developer environment did not provide cl.exe."
    }
    $env:CC = "cl.exe"
    $env:CXX = "cl.exe"
}

Import-MsvcEnvironment

$cmake = (Get-Command cmake -ErrorAction Stop).Source

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

$guiSourceDir = $repoRoot
$guiBuildDir = Join-Path $repoRoot "build"

$coreSourceDir = Join-Path $repoRoot "ScopeOneCore"
$coreBuildDir = Join-Path $coreSourceDir "build"
$coreInstallDir = Join-Path $coreSourceDir "install"
$writerSourceDir = Join-Path $coreSourceDir "external\ScopeWriter"
$writerBuildRoot = Join-Path $writerSourceDir "build"
$writerBuildDir = Join-Path $writerBuildRoot "standalone"
$writerInstallDir = Join-Path $writerSourceDir "install"
$writerConsumerSourceDir = Join-Path $writerSourceDir "tests\consumer"
$writerConsumerBuildDir = Join-Path $writerBuildRoot "consumer"
$config = "Release"
$coreCachePath = Join-Path $coreBuildDir "CMakeCache.txt"
$guiCachePath = Join-Path $guiBuildDir "CMakeCache.txt"

if ($clean) {
    if ($target -eq "scopewriter") {
        if (Test-Path $writerBuildRoot) {
            Write-Step "Removing ScopeWriter build directory"
            Remove-Item -LiteralPath $writerBuildRoot -Recurse -Force
        }
        if (Test-Path $writerInstallDir) {
            Write-Step "Removing ScopeWriter install directory"
            Remove-Item -LiteralPath $writerInstallDir -Recurse -Force
        }
    }
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

if ($target -eq "scopewriter") {
    $writerGeneratorArgs = @()
    if ($env:OS -eq "Windows_NT") {
        $writerGeneratorArgs = @("-G", "Visual Studio 17 2022", "-A", "x64")
    }
    Invoke-Step `
        -Label "Configuring standalone ScopeWriter" `
        -FilePath $cmake `
        -Arguments (@(
            "-S", $writerSourceDir,
            "-B", $writerBuildDir,
            "-DCMAKE_INSTALL_PREFIX=$writerInstallDir",
            "-DSCOPEWRITER_BUILD_TESTS=ON"
        ) + $writerGeneratorArgs) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Building standalone ScopeWriter ($config)" `
        -FilePath $cmake `
        -Arguments @(
            "--build", $writerBuildDir,
            "--config", $config,
            "--parallel"
        ) `
        -WorkingDirectory $repoRoot

    $ctest = (Get-Command ctest -ErrorAction Stop).Source
    Invoke-Step `
        -Label "Testing standalone ScopeWriter ($config)" `
        -FilePath $ctest `
        -Arguments @(
            "--test-dir", $writerBuildDir,
            "-C", $config,
            "--output-on-failure"
        ) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Installing standalone ScopeWriter" `
        -FilePath $cmake `
        -Arguments @(
            "--install", $writerBuildDir,
            "--config", $config
        ) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Configuring installed ScopeWriter consumer" `
        -FilePath $cmake `
        -Arguments (@(
            "-S", $writerConsumerSourceDir,
            "-B", $writerConsumerBuildDir,
            "-DCMAKE_PREFIX_PATH=$writerInstallDir"
        ) + $writerGeneratorArgs) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Building installed ScopeWriter consumer ($config)" `
        -FilePath $cmake `
        -Arguments @(
            "--build", $writerConsumerBuildDir,
            "--config", $config,
            "--parallel"
        ) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Testing installed ScopeWriter consumer ($config)" `
        -FilePath $ctest `
        -Arguments @(
            "--test-dir", $writerConsumerBuildDir,
            "-C", $config,
            "--output-on-failure"
        ) `
        -WorkingDirectory $repoRoot
}

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
                "--install", $coreBuildDir,
                "--config", $config
            ) `
            -WorkingDirectory $repoRoot
    }
}

if ($target -in @("all", "gui")) {
    if ($env:OS -eq "Windows_NT") {
        $guiBuildPrefix = [System.IO.Path]::GetFullPath($guiBuildDir).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
        $mcpProcesses = @(Get-Process -Name "ScopeOneMcpServer" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Path -and $_.Path.StartsWith(
                    $guiBuildPrefix,
                    [System.StringComparison]::OrdinalIgnoreCase)
            })
        if ($mcpProcesses.Count -gt 0) {
            Write-Step "Stopping build-tree ScopeOne MCP server"
            $mcpProcesses | Stop-Process -Force
            Wait-Process -Id $mcpProcesses.Id -Timeout 5 -ErrorAction SilentlyContinue
        }
    }

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
$packageCandidates = Get-ChildItem -LiteralPath $guiBuildDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "ScopeOne-*-win-x64.zip" -or $_.Name -like "ScopeOne-*-win-x64.exe" } |
    Sort-Object LastWriteTime -Descending

Write-Step "Summary"
Write-Host "Target: $target"
Write-Host "Config: $config"
if ($target -eq "scopewriter") {
    Write-Host "ScopeWriter install: $writerInstallDir"
}
else {
    Write-Host "ScopeOneCore install: $coreInstallDir"
}
if ($target -ne "scopewriter") {
    if (Test-Path $guiExe) {
        Write-Host "GUI executable: $guiExe"
    }
    if ($packageCandidates) {
        Write-Host "Packages:"
        foreach ($candidate in $packageCandidates) {
            Write-Host "  $($candidate.FullName)"
        }
    }
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
