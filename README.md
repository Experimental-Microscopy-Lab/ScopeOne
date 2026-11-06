<p align="center">
  <img src="resources/Scopeone_Logo.svg" width="400">
</p>

<p align="center">
  <a href="https://github.com/Experimental-Microscopy-Lab/ScopeOne/actions/workflows/compile-check.yml"><img src="https://github.com/Experimental-Microscopy-Lab/ScopeOne/actions/workflows/compile-check.yml/badge.svg?branch=main" alt="Compile Check"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-BSD--3--Clause-blue.svg" alt="BSD 3-Clause License"></a>
  <!-- <a href="https://doi.org/10.48550/arXiv.2606.19384"><img src="https://img.shields.io/badge/arXiv-2606.19384-b31b1b.svg" alt="Preprint DOI"></a> -->
</p>

ScopeOne is a high-performance open-source microscopy control platform built with C++ and Qt. It is designed to be an extensible platform for advanced microscopy applications, including multiple camera live imaging, on-the-fly image processing, and automated experiments.
 It retains compatibility with the [Micro-Manager](https://micro-manager.org/) broad device ecosystem while allowing featuers like online image processing with GPU acceleration, deep learning inference and more.
<p align="center">
  <img src="resources/Screenshot 2026-09-01 140040.png" width="720"><br>
  <sub> Graphical User Interface of ScopeOne</sub>
</p>

As an open-source project, ScopeOne builds on existing community efforts to reduce duplicated work and provides an alternative that enriches the microscopy community. While the current development is conducted in close collaboration with the optics and biology teams within our laboratory, we aim to expand engagement with the broader research community to make the platform more practical, accessible, and universal. Any issues or pull requests are greatly appreciated！

## ✨ New Features

### Deep Learning Inference

Deep learning is becoming an increasingly powerful tool for microscopy image restoration and analysis. ScopeOne allows users to import pre-trained AI models through the widely supported ONNX model format and apply them directly to live camera streams or recorded images. Models can run on either the CPU or GPU.

<p align="center">
  <img src="resources/ONNX.png" width="720"><br>
  <sub>Real-time denoising demo using <a href="https://huggingface.co/qualcomm/DnCNN">DnCNN</a> at approximately 150 FPS for 256 × 256 images, tested on an NVIDIA RTX 4070 Ti</sub>
</p>

### GPU-Accelerated Online Image Processing
Despite the deep learning, traditional image processing methods remain essential. GPU-accelerated image processing pipeline can be applied to live camera streams or recorded images, including filtering, background subtraction, FFT, and more. Users can customize image processing algorithms and parameters to suit their specific needs with Python processing modules.
## 🚀 Quick Start

Download the latest release installer from the [Releases](https://github.com/Experimental-Microscopy-Lab/ScopeOne/releases) page.

**System Requirements:**

- Windows 10/11 (64-bit)
- Micro-Manager device adapters for your hardware

**Device Adapter Setup:**

ScopeOne loads Micro-Manager configuration files (.cfg) directly and includes the basic adapters required for its demo configurations. To access additional hardware adapters, install [Micro-Manager 2.0](https://download.micro-manager.org/nightly/2.0/Windows/). ScopeOne automatically detects the standard `C:\Program Files\Micro-Manager-2.0` installation. If you installed Micro-Manager in a different location, select `Tools → Settings...` in ScopeOne, then set `Micro-Manager Directory` to the folder containing the Micro-Manager device adapter files, typically named `mmgr_dal_*.dll`. Bundled adapters take precedence over external adapters. We recommend confirming that hardware works in the selected Micro-Manager installation before using it in ScopeOne.

**Dual-camera Setup:**

There is an example dual-camera .cfg file in the config folder, just change the camera labels in Devices section to match your camera models.

## 🛠️ For Developers

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing the code.

**Prerequisites:**

- Git, for the ScopeWriter submodule and Micro-Manager
- A C++20 compiler: [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) or its Build Tools (MSVC v143 toolset) on Windows, GCC or Clang on Linux and macOS (experimental)
- [CMake](https://cmake.org/download/) 3.23 or newer, which comes with the C++ tools of Visual Studio on Windows
- Python 3.14, only for the Python package and Python processing modules

The build scripts set up the rest. They download [vcpkg](https://github.com/microsoft/vcpkg) when `VCPKG_ROOT` does not name a checkout, and vcpkg builds Qt 6.11.2, OpenCV 4.14.0 and ScopeWriter's libtiff, zlib, zstd and crc32c inside the repository. They also fetch and build Micro-Manager's core with the DemoCamera and Utilities adapters.

Clone ScopeOne with its submodules, or initialize them in an existing checkout:

```powershell
git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
git submodule update --init --recursive
```

### Drivers and processing modules

ScopeOne has two public extension points, drivers and Python processing modules, and both run outside the ScopeOne process. Drivers are loaded from `drivers` by `ScopeOne_DriverHost`, one process per device, and Python modules from `processing/python`. The CUDA and ONNX processing modules are part of Core. Core builds the CUDA modules when it finds nvcc, and ONNX inference always, with the GPU package of ONNX Runtime when CUDA is built and the CPU package otherwise. `-DSCOPEONE_ENABLE_CUDA=OFF` builds the standard Core and `-DSCOPEONE_ENABLE_INFERENCE=OFF` leaves ONNX out. Micro-Manager remains the built-in camera provider and loads its device adapters from `.cfg` files. [drivers/README.md](drivers/README.md) explains how to write a driver and [ScopeOneCore/python/scopeone/README.md](ScopeOneCore/python/scopeone/README.md#python-processing-modules) how to write a Python processing module.

**Windows Build Steps:**

On a new Windows machine, install Git and the Visual Studio 2022 Build Tools with the C++ workload, which bring MSVC, MSBuild, the Windows SDK and CMake. An existing Visual Studio 2022 with "Desktop development with C++" works as well.

```powershell
winget install Git.Git
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait"
```

`winget` is the package manager included in Windows 10 and 11. The Build Tools are Visual Studio without the IDE, free to use. `--override` passes its options to the Visual Studio installer: `--add ...VCTools` selects the C++ build tools, `--includeRecommended` adds the Windows SDK and CMake, `--passive` shows only a progress bar, and `--wait` returns once the installation has finished. The installer asks for administrator rights, downloads a few gigabytes and takes 10 to 20 minutes.

Then clone ScopeOne and build and start it with one command, in a new terminal so Git is on the path:

```powershell
git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
cd ScopeOne
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --run
```

Windows does not run `.ps1` files by default. Its execution policy only guards against starting a script by accident, and `-ExecutionPolicy Bypass` lifts it for this one run without changing any setting of the machine. Use the same command for every build, or run `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` once, which needs no administrator rights, and then start the script as `.\scripts\build.ps1`. 

The script finds Visual Studio itself, so any terminal works. If `VCPKG_ROOT` is not set, it offers to download vcpkg into `%USERPROFILE%\vcpkg` and saves `VCPKG_ROOT` for your user account, and `--yes` accepts without asking. It clones the latest mmCoreAndDevices and builds MMCore with the DemoCamera and Utilities adapters, then ScopeOneCore, the drivers and the GUI. The first build compiles Qt, OpenCV and their dependencies with vcpkg, which takes about an hour on a four core machine, four fifths of it for Qt. vcpkg keeps the result in `vcpkg_installed\` and in its binary cache, so later builds only compile ScopeOne.

Later builds run the same command without `--run`, or with it to start ScopeOne afterwards. `--run` starts ScopeOne after the build, `--configure` configures again, `--clean` discards the build directories, and `--target core`, `gui` or `drivers` builds one part. Open `config\MMConfig_demo.cfg` in ScopeOne to try it without hardware. The first build downloads ONNX Runtime into `ScopeOneCore\external`, the GPU package when the CUDA Toolkit is installed. The last lines of the build show whether Core was built with the CUDA and ONNX modules.

For a direct Core CMake configure, after the script has built MMCore once (`--target core`):

```powershell
cmake -S ScopeOneCore -B ScopeOneCore/build `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_MANIFEST_DIR=$PWD" `
  "-DVCPKG_INSTALLED_DIR=$PWD/vcpkg_installed" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

The built application is `build\Release\ScopeOne.exe`.

**Linux and macOS Build Steps:**

Micro-Manager is built from its own checkout because its native core and device adapters are not vcpkg dependencies. Everything else comes from vcpkg with the dynamic triplet of the host, so Qt is shared between the GUI, ScopeOneCore and the drivers.

1. Install the system build tools.

Linux:

```bash
sudo apt install \
  git build-essential cmake autoconf automake libtool autoconf-archive \
  pkg-config ninja-build curl zip unzip libboost-all-dev
sudo apt install --no-upgrade \
  libegl-dev libopengl-dev libx11-xcb-dev libxrender-dev \
  libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-keysyms1-dev \
  libxcb-randr0-dev libxcb-sync-dev libxcb-xinput-dev libxcb-glx0-dev \
  libxkbcommon-x11-dev
```

The second command installs the X11 and OpenGL development files that vcpkg's `qtbase` port expects from the system. It is not a system Qt installation, and `--no-upgrade` keeps it from upgrading the installed Mesa drivers.

macOS:

```bash
xcode-select --install
brew install \
  git cmake autoconf automake libtool autoconf-archive \
  pkg-config ninja boost
```

2. Build from the ScopeOne repository root:

```bash
./scripts/build.sh
```

If vcpkg is missing, the script offers to download it into `$HOME/vcpkg` or a folder you type, and `--yes` takes the default folder without asking. The first build compiles Qt, OpenCV and their dependencies with vcpkg, about 160 CPU minutes, 11 minutes on a 48 core Linux machine and about 40 on four cores, and later builds reuse them from the binary cache. It then builds Micro-Manager's core with the DemoCamera and Utilities adapters, ScopeOneCore, the drivers and the GUI. Set `JOBS` to limit parallel jobs and `BUILD_TYPE` to change the CMake build type (default `Release`). Pass `--clean` to discard the previous build directories. The first build downloads ONNX Runtime into `ScopeOneCore/external`.

The vcpkg packages of a first build without the binary cache, measured on a 48 core Linux machine:

| Package | Time |
|---|---|
| qtbase | 5.2 min |
| opencv4 | 2.2 min |
| qtsvg | 59 s |
| harfbuzz | 46 s |
| gperf, libb2, zstd, pcre2 | 10 to 22 s each |
| 29 others | under 10 s each |
| All 37 packages | 11 min 23 s |

The executable is `build/ScopeOne`, and `config/MMConfig_demo.cfg` runs with the copied demo adapters. It loads Qt and OpenCV from `vcpkg_installed/<triplet>/lib`, so run it from the build tree.

## 🤖 Automation and AI Agents

The desktop app exposes a language-neutral local control API and shared-memory frame channel. An AI agent does not run inside ScopeOne or depend on Python. A tool adapter can discover supported operation groups with the `capabilities` request, read a structured observation with `state_snapshot`, and invoke the exposed camera, stage, mosaic, processing, image analysis, experiment, recording, layer, and markup operations. Requests may carry an ID that is echoed by the app for correlation. Recording save operations use `ome-tiff` by default and also accept `ome-zarr`, `tiff` and `binary`.

The API reports which operations mutate hardware, write files, remove state, or may run for a long time. An agent adapter should request user confirmation before those operations and verify the result with the returned read-back value, experiment status, or a new state snapshot. The Python package is one optional client implementation. See the [Python client and Local API protocol guide](ScopeOneCore/python/scopeone/README.md) for protocol details and runnable examples.

### MCP adapter

`ScopeOneMcpServer` is a standalone C++ MCP server included with ScopeOne. It uses MCP protocol version `2025-06-18` over standard input and output and forwards validated tool calls to the running desktop app through the Local API. It does not embed a model, open a network port, or depend on Python.

#### Claude Code plugin

Install ScopeOne first, then run these commands inside Claude Code:

```text
/plugin marketplace add Experimental-Microscopy-Lab/ScopeOne-Skill
/plugin install scopeone@scopeone
/reload-plugins
```

When prompted, select `ScopeOneMcpServer.exe` from the same directory as `ScopeOne.exe`. Start ScopeOne before asking Claude to inspect or control it. The plugin supplies the MCP configuration and agent operating guidance, so no Python installation or manual MCP server launch is required.

#### Other MCP hosts

To use it:

1. Start ScopeOne. Load the device configuration manually or ask the agent to load it after confirmation.
2. Configure an MCP-compatible agent host to launch `ScopeOneMcpServer.exe` from the ScopeOne installation or portable package directory using the `stdio` transport.
3. Ask the agent to inspect ScopeOne state before requesting an action.
4. Review hardware or destructive tool calls before allowing the adapter's required `confirm=true` argument.

Use `scopeone` as the server name, `stdio` as the transport, the absolute path to `ScopeOneMcpServer.exe` as the command, and no command-line arguments. The exact configuration syntax depends on the agent host.

The MCP tool set mirrors the Local API operation catalog, including system state, configuration, preview layers, independent image windows, automatic display levels, source alignment, markups, device properties, exposure, ROI, stages, stage mosaics, processing, experiments, recording sessions, frame transfer, and analysis. Agents can list, open, activate, process, save, and close independent image windows, read the current frame of any image layer, monitor live acquisition and writer progress, and optionally export or display particle masks. ScopeOne remains the authority for parameter validation and hardware read-back, and MCP tool calls are visible in the desktop UI through the same application state used by manual controls.

Configuration loading and unloading report an explicit lifecycle state through the Local API and MCP. A configuration with non-camera initialization warnings is reported as `partially_loaded` with failed device labels; camera backend startup failures are cleaned up and reported as errors. During `loading` or `unloading`, hardware mutations are rejected until the operation finishes.

## 🔬 Tested Devices

- Yokogawa CSU X1
- Hamamatsu C13440
- Andor 897D

The current validation list is still short, but the codebase has been cleaned to remove early hard-coded device assumptions. In principle, ScopeOne should follow Micro-Manager device compatibility.

## 🖥️ Tested System Configurations

- Windows 10 Version 21H2, Dual Intel(R) Xeon(R) E5-2637 v3, 64 GB RAM, NVIDIA Quadro K620
- Windows 11 Version 25H2, Intel(R) Core(TM) Ultra 5 125U, 64 GB RAM
- Fedora Linux 44 (Workstation Edition), Intel(R) Core(TM) i7-7700, 32 GB RAM
- macOS 26.5.2, Apple M1 Pro, 16 GB RAM

We build and test ScopeOne on the above machines, which are comparatively older and weaker than many typical optical lab computers. However, ScopeOne still provides smooth real-time preview and processing on them.
