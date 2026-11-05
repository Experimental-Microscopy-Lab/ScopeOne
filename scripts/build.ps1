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
$assumeYes = $false

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
        { $_ -in @("--yes", "-yes", "-y") } {
            $assumeYes = $true
            continue
        }
        default {
            throw "Unknown option: $arg"
        }
    }
}

if ($target -notin @("all", "core", "gui", "drivers", "scopewriter")) {
    throw "Invalid target '$target'. Expected one of: all, core, gui, drivers, scopewriter."
}
if ($package -and $target -in @("core", "drivers", "scopewriter")) {
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
        throw "Visual Studio 2022 with the C++ tools was not found.`n$visualStudioInstallHint"
    }
    $installationPath = & $vsWhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
        throw "Visual Studio is installed without the C++ tools.`n$visualStudioInstallHint"
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

# The Build Tools bring MSVC, MSBuild, the Windows SDK and CMake, which is all the build needs
# besides Git
$visualStudioInstallHint = @"
Install the Visual Studio 2022 Build Tools with the C++ workload and run this script again:

  winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait"

An existing Visual Studio 2022 works too once "Desktop development with C++" is added in the Visual Studio Installer.
"@

# Clones vcpkg into the user's folder when VCPKG_ROOT does not name one, after asking, and
# bootstraps it. vcpkg builds Qt, OpenCV and the other libraries inside the repository.
function Resolve-Vcpkg {
    $root = $env:VCPKG_ROOT
    if (-not $root -or -not (Test-Path (Join-Path $root "scripts\buildsystems\vcpkg.cmake"))) {
        $root = Join-Path $env:USERPROFILE "vcpkg"
        if (-not (Test-Path (Join-Path $root "scripts\buildsystems\vcpkg.cmake"))) {
            if (-not $assumeYes) {
                if ([Console]::IsInputRedirected) {
                    throw "Set VCPKG_ROOT to a bootstrapped vcpkg checkout, or pass --yes to download vcpkg into $root."
                }
                Write-Host "vcpkg is a C++ package manager. ScopeOne uses it to download and build Qt, OpenCV and its other libraries, and nothing is installed system wide."
                $answer = Read-Host "vcpkg was not found. Download it into $root? Press Enter to accept, type another folder, or n to cancel"
                if ($answer -in @("n", "N", "no", "No")) {
                    throw "Set VCPKG_ROOT to a bootstrapped vcpkg checkout and run this script again."
                }
                if ($answer) {
                    $root = $answer
                }
            }
            if (-not (Test-Path (Join-Path $root "scripts\buildsystems\vcpkg.cmake"))) {
                Invoke-Step `
                    -Label "Downloading vcpkg" `
                    -FilePath $git `
                    -Arguments @("clone", "https://github.com/microsoft/vcpkg.git", $root) `
                    -WorkingDirectory $env:USERPROFILE
            }
        }
        $env:VCPKG_ROOT = $root
        [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $root, "User")
        Write-Host "Using vcpkg in $root, saved as VCPKG_ROOT for your user account."
    }
    if (-not (Test-Path (Join-Path $root "vcpkg.exe"))) {
        Invoke-Step `
            -Label "Bootstrapping vcpkg" `
            -FilePath (Join-Path $root "bootstrap-vcpkg.bat") `
            -Arguments @("-disableMetrics") `
            -WorkingDirectory $root
    }
    return $root
}

# Clones the latest mmCoreAndDevices and builds MMCore and the two adapters the demo
# configurations use. Core links MMCore.lib and copies the adapters next to
# ScopeOne. An existing checkout is used as it is, and nothing is rebuilt once its outputs exist.
function Ensure-MMCore {
    $mm = Join-Path $coreSourceDir "external\mmCoreAndDevices"
    $binDir = Join-Path $mm "build\Release\x64"
    $missing = @(@("MMCore.lib", "mmgr_dal_DemoCamera.dll", "mmgr_dal_Utilities.dll") |
        Where-Object { -not (Test-Path (Join-Path $binDir $_)) })
    if ($missing.Count -eq 0) {
        return
    }
    if (-not (Test-Path (Join-Path $mm "MMCore\MMCore.vcxproj"))) {
        Invoke-Step -Label "Downloading mmCoreAndDevices" -FilePath $git `
            -Arguments @("clone", "--depth", "1",
                "https://github.com/micro-manager/mmCoreAndDevices.git", $mm) `
            -WorkingDirectory $repoRoot
    }
    $msbuild = (Get-Command msbuild -ErrorAction Stop).Source
    foreach ($project in @(
            "MMCore\MMCore.vcxproj",
            "DeviceAdapters\DemoCamera\DemoCamera.vcxproj",
            "DeviceAdapters\Utilities\Utilities.vcxproj")) {
        Invoke-Step `
            -Label "Building $project" `
            -FilePath $msbuild `
            -Arguments @(
                (Join-Path $mm $project),
                "/p:Configuration=Release",
                "/p:Platform=x64",
                "/p:SolutionDir=$mm\",
                "/m",
                "/nologo",
                "/v:minimal"
            ) `
            -WorkingDirectory $mm
    }
}

# Prints one line per tool, a check mark or a cross with what it is for, and stops after the list
# with the install commands of everything missing. The marks are built from code points, as
# Windows PowerShell reads a script without a byte order mark in the legacy code page.
$checkMark = [string][char]0x2713
$crossMark = [string][char]0x2717
$missingTools = @()
function Write-ToolLine {
    param([bool]$Found, [string]$Label, [string]$Detail)

    Write-Host "  " -NoNewline
    if ($Found) {
        Write-Host $checkMark -ForegroundColor Green -NoNewline
    }
    else {
        Write-Host $crossMark -ForegroundColor Red -NoNewline
    }
    Write-Host (" {0,-14} {1}" -f $Label, $Detail)
}

Write-Step "Checking build tools"
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
if ($gitCommand) {
    $git = $gitCommand.Source
    Write-ToolLine $true "Git" ((& $git --version) -join " ")
}
else {
    Write-ToolLine $false "Git" "fetches the ScopeWriter submodule, vcpkg and Micro-Manager"
    $missingTools += "Git"
}

$msvcError = $null
try {
    Import-MsvcEnvironment
    Write-ToolLine $true "Visual Studio" ("MSVC $env:VCToolsVersion, Windows SDK $env:WindowsSDKVersion".TrimEnd("\"))
}
catch {
    $msvcError = $_.Exception.Message
    Write-ToolLine $false "Visual Studio" "compiles everything with MSVC"
    $missingTools += "Visual Studio"
}

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$msbuildCommand = Get-Command msbuild -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
    Write-ToolLine $true "CMake" ((& $cmake --version) | Select-Object -First 1)
}
else {
    Write-ToolLine $false "CMake" "configures ScopeOne, part of the C++ tools of Visual Studio"
    $missingTools += "CMake"
}
if ($msbuildCommand) {
    Write-ToolLine $true "MSBuild" "builds Micro-Manager's core and adapters"
}
else {
    Write-ToolLine $false "MSBuild" "builds Micro-Manager's core and adapters, part of Visual Studio"
    $missingTools += "MSBuild"
}

if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
    Write-ToolLine $true "vcpkg" $env:VCPKG_ROOT
}
else {
    Write-Host "  - " -ForegroundColor Yellow -NoNewline
    Write-Host ("{0,-14} {1}" -f "vcpkg", "not set up yet, offered below")
}

if ($missingTools.Count -gt 0) {
    $hints = @()
    if ("Git" -in $missingTools) {
        $hints += "Install Git:`n`n  winget install Git.Git`n"
    }
    if ($missingTools | Where-Object { $_ -ne "Git" }) {
        if ($msvcError) {
            $hints += $msvcError
        }
        else {
            $hints += $visualStudioInstallHint
        }
    }
    throw "Missing build tools: $($missingTools -join ', ')`n`n$($hints -join "`n")`nOpen a new terminal after installing, so the tools are on the path."
}

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
$driverInstallDir = Join-Path $coreInstallDir "bin"
# Inside drivers rather than build, whose drivers folder is where the GUI build puts the drivers it runs
$driverSourceDir = Join-Path $repoRoot "drivers"
$driverBuildDir = Join-Path $driverSourceDir "build"
$config = "Release"
$coreCachePath = Join-Path $coreBuildDir "CMakeCache.txt"
$guiCachePath = Join-Path $guiBuildDir "CMakeCache.txt"
$driverCachePath = Join-Path $driverBuildDir "CMakeCache.txt"

$vcpkgConfigureArgs = @()
$writerVcpkgArgs = @()
if ($env:OS -eq "Windows_NT") {
    $vcpkgToolchain = Join-Path (Resolve-Vcpkg) "scripts\buildsystems\vcpkg.cmake"
}
if ($env:OS -eq "Windows_NT" -and $target -eq "scopewriter") {
    # Standalone ScopeWriter installs its own vcpkg.json dependencies and ships their DLLs
    $writerVcpkgArgs = @(
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON"
    )
}
elseif ($env:OS -eq "Windows_NT") {
    $vcpkgConfigureArgs = @(
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
        "-DVCPKG_MANIFEST_DIR=$repoRoot",
        "-DVCPKG_INSTALLED_DIR=$(Join-Path $repoRoot 'vcpkg_installed')",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-UTiff_DIR",
        "-UOpenCV_DIR",
        "-UQt6*_DIR",
        "-UWINDEPLOYQT_EXECUTABLE"
    )
}

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
    if ($target -in @("all", "drivers") -and (Test-Path $driverBuildDir)) {
        Write-Step "Removing driver build directory"
        Remove-Item -LiteralPath $driverBuildDir -Recurse -Force
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
$driverBuildFilesExist = (Test-Path (Join-Path $driverBuildDir "ALL_BUILD.vcxproj")) -or
    (Test-Path (Join-Path $driverBuildDir "build.ninja")) -or
    (Test-Path (Join-Path $driverBuildDir "Makefile"))
$needDriverConfigure = $configure -or -not (Test-Path $driverCachePath) -or -not $driverBuildFilesExist
$driverCachedInstallPrefix = Normalize-CMakePath (
    Get-CMakeCacheValue -CachePath $driverCachePath -Key "CMAKE_INSTALL_PREFIX")
if ($driverCachedInstallPrefix -and
    $driverCachedInstallPrefix -ne (Normalize-CMakePath $driverInstallDir)) {
    $needDriverConfigure = $true
}

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
        ) + $writerVcpkgArgs + $writerGeneratorArgs) `
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
    if ($env:OS -eq "Windows_NT") {
        Ensure-MMCore
    }
    if (-not (Test-Path (Join-Path $repoRoot "vcpkg_installed\x64-windows"))) {
        Write-Host ""
        Write-Host "The first build compiles Qt and OpenCV with vcpkg, which takes about an hour. Later builds reuse the result." -ForegroundColor Yellow
    }

    # Core builds the CUDA processing modules, which need the CUDA toolset. A generator keeps the
    # toolset it was first configured with, so a build directory made without it is configured
    # again from scratch.
    $coreToolset = if ($env:CUDA_PATH) { "cuda=$env:CUDA_PATH" } else { "" }
    if ($coreToolset -and (Test-Path $coreCachePath) -and
        (Get-CMakeCacheValue -CachePath $coreCachePath -Key "CMAKE_GENERATOR_TOOLSET") -ne $coreToolset) {
        Write-Step "Removing ScopeOneCore build directory configured without the CUDA toolset"
        Remove-Item -LiteralPath $coreBuildDir -Recurse -Force
        $needCoreConfigure = $true
    }

    if ($needCoreConfigure) {
        $coreConfigureArgs = @(
            "-S", $coreSourceDir,
            "-B", $coreBuildDir
        )

        if (-not $installPrefixOverride) {
            $coreConfigureArgs += "-DCMAKE_INSTALL_PREFIX=$coreInstallDir"
        }

        $coreConfigureArgs += $vcpkgConfigureArgs + $coreConfigureOption
        if ($coreToolset) {
            $coreConfigureArgs += @("-T", $coreToolset)
        }

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

if ($target -in @("all", "drivers")) {
    $coreConfigFile = Join-Path $coreInstallDir "lib\cmake\ScopeOneCore\ScopeOneCoreConfig.cmake"
    if (-not (Test-Path $coreConfigFile)) {
        throw "ScopeOneCore is not installed at $coreInstallDir. Build the core first."
    }

    if ($needDriverConfigure) {
        Invoke-Step `
            -Label "Configuring ScopeOne drivers" `
            -FilePath $cmake `
            -Arguments (@(
                "-S", $driverSourceDir,
                "-B", $driverBuildDir,
                "-DScopeOneCore_ROOT=$coreInstallDir",
                "-DCMAKE_PREFIX_PATH=$coreInstallDir",
                "-DCMAKE_INSTALL_PREFIX=$driverInstallDir"
            ) + $vcpkgConfigureArgs) `
            -WorkingDirectory $repoRoot
    }

    Invoke-Step `
        -Label "Building ScopeOne drivers ($config)" `
        -FilePath $cmake `
        -Arguments @(
            "--build", $driverBuildDir,
            "--config", $config,
            "--parallel"
        ) `
        -WorkingDirectory $repoRoot

    Invoke-Step `
        -Label "Installing ScopeOne drivers" `
        -FilePath $cmake `
        -Arguments @(
            "--install", $driverBuildDir,
            "--config", $config
        ) `
        -WorkingDirectory $repoRoot

    # The GUI decides at configure time whether there is a drivers folder to copy, so it is
    # configured again once the drivers are installed
    if ($target -eq "all") {
        $needGuiConfigure = $true
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
        ) + $vcpkgConfigureArgs + $guiConfigureOption

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
    Where-Object { $_.Name -like "ScopeOne-*-win-x64*.zip" -or $_.Name -like "ScopeOne-*-win-x64*.exe" } |
    Sort-Object LastWriteTime -Descending

Write-Step "Summary"
Write-Host "Target: $target"
Write-Host "Config: $config"
if ($target -eq "scopewriter") {
    Write-Host "ScopeWriter install: $writerInstallDir"
}
else {
    Write-Host "ScopeOneCore install: $coreInstallDir"
    # Core builds the CUDA and ONNX modules only when it finds their toolkits, so a missing one
    # leaves them out without failing the build
    if (Test-Path $coreCachePath) {
        Write-Host -NoNewline "Core modules: CUDA: "
        foreach ($module in @(
                @{ Key = "SCOPEONE_BUILT_WITH_CUDA"; Next = ", ONNX: " },
                @{ Key = "SCOPEONE_BUILT_WITH_INFERENCE"; Next = "" })) {
            if ((Get-CMakeCacheValue -CachePath $coreCachePath -Key $module.Key) -eq "ON") {
                Write-Host -NoNewline "on" -ForegroundColor Green
            }
            else {
                Write-Host -NoNewline "off" -ForegroundColor Yellow
            }
            Write-Host -NoNewline $module.Next
        }
        Write-Host ""
    }
    if ($target -in @("all", "drivers")) {
        Write-Host "Drivers: $driverBuildDir"
    }
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
