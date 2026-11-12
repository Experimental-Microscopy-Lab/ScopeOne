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
- For the GPU package, an NVIDIA GPU; ONNX inference on CUDA also needs cuDNN 9 and the cuBLAS of CUDA 12 on the `PATH`, which the package does not include

**Device Adapter Setup:**

ScopeOne loads Micro-Manager configuration files (.cfg) directly and includes the basic adapters required for its demo configurations. To access additional hardware adapters, install [Micro-Manager 2.0](https://download.micro-manager.org/nightly/2.0/Windows/). ScopeOne automatically detects the standard `C:\Program Files\Micro-Manager-2.0` installation. If you installed Micro-Manager in a different location, select `Tools → Settings...` in ScopeOne, then set `Micro-Manager Directory` to the folder containing the Micro-Manager device adapter files, typically named `mmgr_dal_*.dll`. Bundled adapters take precedence over external adapters. We recommend confirming that hardware works in the selected Micro-Manager installation before using it in ScopeOne.

**Dual-camera Setup:**

There is an example dual-camera .cfg file in the config folder, just change the camera labels in Devices section to match your camera models.

## 🛠️ For Developers

**TL;DR.** On a fresh machine, without spending time on setting up the C++ environment, run these commands. They install the tools, clone ScopeOne, and let the script build everything else, and start ScopeOne. The first build takes long, see [Build times](#build-times).

Windows:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait"
git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
cd ScopeOne
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --run
```

Linux (Debian, Ubuntu):

```bash
sudo apt install \
  build-essential cmake autoconf automake libtool autoconf-archive \
  pkg-config ninja-build curl zip unzip libboost-all-dev
sudo apt install --no-upgrade \
  libegl-dev libopengl-dev libx11-xcb-dev libxrender-dev \
  libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-keysyms1-dev \
  libxcb-randr0-dev libxcb-sync-dev libxcb-xinput-dev libxcb-glx0-dev \
  libxkbcommon-x11-dev
git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
cd ScopeOne
./scripts/build.sh --run
```

macOS:

```bash
xcode-select --install
brew install cmake autoconf automake libtool autoconf-archive pkg-config ninja boost
git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
cd ScopeOne
./scripts/build.sh --run
```

A coding agent such as Claude Code or Codex can also do this: ask it to set up and build ScopeOne by following this README.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing the code.

### Qt

ScopeOne needs Qt 6.8 or later. There are three ways to get it.

- **Qt is already installed.** Pass its directory once and the script uses it instead of building Qt:

  ```powershell
  # Windows
  powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --qt C:\Qt\6.11.2\msvc2022_64
  # Linux and macOS
  ./scripts/build.sh --qt ~/Qt/6.11.2/gcc_64   # macOS: ~/Qt/6.11.2/macos
  ```

  On Windows the script saves the path as `SCOPEONE_QT_DIR` for your user account, and `--qt vcpkg` goes back to the Qt that vcpkg builds. On Linux and macOS, export `SCOPEONE_QT_DIR` with that path to keep it without `--qt`.

- **Install Qt yourself** with the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss), then pass its directory as above. On Windows choose the MSVC 2022 64-bit build; MinGW does not work.

- **Let the script build it.** Without `--qt`, vcpkg builds Qt on the first build, see [Build times](#build-times).

### Windows, step by step

1. **Install the C++ build tools.** In a terminal:

   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait"
   ```

   `winget` comes with Windows 10 and 11. The command installs MSVC, the Windows SDK and CMake. It asks for administrator rights, downloads a few gigabytes and takes 10 to 20 minutes. An existing Visual Studio 2022 with "Desktop development with C++" works as well.

   A Qt passed with `--qt` needs the Qt Serial Port module, which the serial driver links.

2. **Clone ScopeOne.**

   ```powershell
   git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
   cd ScopeOne
   ```

3. **Build and start it.**

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --run
   ```

   Windows blocks `.ps1` scripts by default, and `-ExecutionPolicy Bypass` lifts that for this one command. To lift it for good, run `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` once; the script then starts as `.\scripts\build.ps1 --run`.

   The script finds Visual Studio itself. It offers to download vcpkg into `%USERPROFILE%\vcpkg` and saves `VCPKG_ROOT` for your user account, and `--yes` accepts without asking. It then builds MMCore with the DemoCamera and Utilities adapters, ScopeOneCore, the drivers and the GUI.

   The first build takes long, see [Build times](#build-times). The last lines of the build show whether Core was built with the CUDA and ONNX modules.

4. **Try it without hardware.** Open `config\MMConfig_demo.cfg` in ScopeOne. The application is `build\Release\ScopeOne.exe`.

5. **Build again after a change** with the same command. The options are:

   | Option | Effect |
   |---|---|
   | `--run` | Start ScopeOne after the build |
   | `--target core`, `gui` or `drivers` | Build one part |
   | `--configure` | Configure again, for new source files or a newly installed CUDA |
   | `--clean` | Discard the build directories and the Core install and build from scratch |
   | `--package` | Build the installer and zip, named `-gpu` when Core was built with CUDA |
   | `--qt <dir>` | Use an installed Qt, see [Qt](#qt) |
   | `--yes` | Accept the vcpkg download without asking |

### Linux and macOS, step by step

macOS support is experimental.

1. **Install the build tools.**

   Linux (Debian, Ubuntu):

   ```bash
   sudo apt install \
     build-essential cmake autoconf automake libtool autoconf-archive \
     pkg-config ninja-build curl zip unzip libboost-all-dev
   sudo apt install --no-upgrade \
     libegl-dev libopengl-dev libx11-xcb-dev libxrender-dev \
     libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-keysyms1-dev \
     libxcb-randr0-dev libxcb-sync-dev libxcb-xinput-dev libxcb-glx0-dev \
     libxkbcommon-x11-dev
   ```

   The second command installs the X11 and OpenGL development files that vcpkg's Qt needs, and `--no-upgrade` keeps the installed Mesa drivers.

   A Qt passed with `--qt` needs the Qt Serial Port module, which the serial driver links.

   macOS:

   ```bash
   xcode-select --install
   brew install \
     cmake autoconf automake libtool autoconf-archive \
     pkg-config ninja boost
   ```

   A Qt passed with `--qt` needs the Qt Serial Port module, which the serial driver links.

2. **Clone ScopeOne.**

   ```bash
   git clone --recurse-submodules https://github.com/Experimental-Microscopy-Lab/ScopeOne.git
   cd ScopeOne
   ```

3. **Build and start it.**

   ```bash
   ./scripts/build.sh --run
   ```

   The script checks the build tools, offers to download vcpkg into `$HOME/vcpkg` or a folder you type (`--yes` takes the default), and builds Micro-Manager's core with the DemoCamera and Utilities adapters, ScopeOneCore, the drivers and the GUI.

   The first build takes long, see [Build times](#build-times).

4. **Try it without hardware.** The application is `build/ScopeOne`. Run it from the build tree and open `config/MMConfig_demo.cfg`.

5. **Build again after a change** with the same command. `--clean` discards the build directories and the Core install, `--run` starts ScopeOne afterwards, `JOBS` limits the parallel jobs and `BUILD_TYPE` sets the CMake build type (default `Release`).

### Drivers and processing modules

ScopeOne has two public extension points, drivers and Python processing modules, and both run outside the ScopeOne process. Drivers are loaded from `drivers` by `ScopeOne_DriverHost`, one process per device, and Python modules from `processing/python`. The CUDA and ONNX processing modules are part of Core. Core builds the CUDA modules when it finds nvcc, and ONNX inference always, with the GPU package of ONNX Runtime when CUDA is built and the CPU package otherwise. `-DSCOPEONE_ENABLE_CUDA=OFF` builds the standard Core. Micro-Manager remains the built-in camera provider and loads its device adapters from `.cfg` files. [drivers/README.md](drivers/README.md) explains how to write a driver and [ScopeOneCore/python/scopeone/README.md](ScopeOneCore/python/scopeone/README.md#python-processing-modules) how to write a Python processing module. Python 3.14 is needed only for the Python package and Python processing modules.

### Build times

The first build is the long one. vcpkg compiles Qt, OpenCV and their dependencies from source, about 160 CPU minutes, 11 minutes on a 48 core Linux machine, and Qt is the largest part of it. An installed Qt (see [Qt](#qt)) leaves most of it out. vcpkg keeps the compiled packages in its binary cache, so this happens once per machine, and builds after a change take seconds.

The first build without the cache, per vcpkg package, on the 48 core Linux machine:

| Package | Time |
|---|---|
| qtbase | 5.2 min |
| opencv4 | 2.2 min |
| qtsvg | 59 s |
| harfbuzz | 46 s |
| 33 others | under 25 s each |
| All 37 packages | 11 min 23 s |

A fresh clone with the binary cache filled, on the same machine, about 5 minutes in all:

| Step | Time |
|---|---|
| Clone Micro-Manager | 71 s |
| Configure Core: restore the 37 packages from the cache and download ONNX Runtime | 2.6 min |
| Build Core | 28 s |
| Configure and build the drivers | 11 s |
| Configure and build the GUI | 27 s |

Builds after that:

| Change | Time |
|---|---|
| Nothing changed | under 1 s |
| One GUI file | 8 s |
| One Core file | 5 s |

## 🤖 Automation and AI Agents

The desktop app exposes a language-neutral local control API and shared-memory frame channel. A tool adapter can discover supported operation groups with the `capabilities` request, read a structured observation with `state_snapshot`, and invoke the exposed camera, stage, mosaic, processing, image analysis, experiment, recording, layer, and markup operations. Requests may carry an ID that is echoed by the app for correlation. Recording save operations use `ome-tiff` by default and also accept `ome-zarr`, `tiff` and `binary`.

The API reports which operations mutate hardware, write files, remove state, or may run for a long time. An agent adapter should request user confirmation before those operations and verify the result with the returned read-back value, experiment status, or a new state snapshot. The Python package is one optional client implementation. See the [Python client and Local API protocol guide](ScopeOneCore/python/scopeone/README.md) for protocol details and runnable examples.

### MCP adapter

`ScopeOneMcpServer` is a standalone C++ MCP server included with ScopeOne. It uses MCP protocol version `2025-06-18` over standard input and output and forwards validated tool calls to the running desktop app through the Local API.

#### Claude Code plugin

Install ScopeOne first, then run these commands inside Claude Code:

```text
/plugin marketplace add Experimental-Microscopy-Lab/ScopeOne-Skill
/plugin install scopeone@scopeone
/reload-plugins
```

When prompted, select `ScopeOneMcpServer.exe` from the same directory as `ScopeOne.exe`. Start ScopeOne before asking Claude to inspect or control it. The plugin supplies the MCP configuration and the agent guidance.

#### Other MCP hosts

To use it:

1. Start ScopeOne. Load the device configuration manually or ask the agent to load it after confirmation.
2. Configure an MCP-compatible agent host to launch `ScopeOneMcpServer.exe` from the ScopeOne installation or portable package directory using the `stdio` transport.
3. Ask the agent to inspect ScopeOne state before requesting an action.
4. Review hardware or destructive tool calls before allowing the adapter's required `confirm=true` argument.

Use `scopeone` as the server name, `stdio` as the transport, the absolute path to `ScopeOneMcpServer.exe` as the command, and no command-line arguments. The exact configuration syntax depends on the agent host.

The MCP tool set mirrors the Local API operation catalog, including system state, configuration, preview layers, independent image windows, automatic display levels, source alignment, markups, device properties, exposure, ROI, stages, stage mosaics, processing, experiments, recording sessions, frame transfer, and analysis. Agents can list, open, activate, process, save, and close independent image windows, read the current frame of any image layer, monitor live acquisition and writer progress, and optionally export or display particle masks. MCP tool calls are visible in the desktop UI through the same application state used by manual controls.

Configuration loading and unloading report an explicit lifecycle state through the Local API and MCP. A configuration with non-camera initialization warnings is reported as `partially_loaded` with failed device labels; cameras that fail to start are cleaned up and reported as errors. During `loading` or `unloading`, hardware mutations are rejected until the operation finishes.

## 🔬 Tested Devices

- Yokogawa CSU X1
- Hamamatsu C13440
- Andor 897D

Other devices work through the same Micro-Manager device adapters.

## 🖥️ Tested System Configurations

- Windows 10 Version 21H2, Dual Intel(R) Xeon(R) E5-2637 v3, 64 GB RAM, NVIDIA Quadro K620
- Windows 11 Version 25H2, Intel(R) Core(TM) Ultra 5 125U, 64 GB RAM
- Fedora Linux 44 (Workstation Edition), Intel(R) Core(TM) i7-7700, 32 GB RAM
- macOS 26.5.2, Apple M1 Pro, 16 GB RAM


ScopeOne is built and tested on the above machines, which are comparatively older and weaker than many typical optical lab computers. However, ScopeOne still provides smooth real-time preview and processing on them, as an example, with a Hamamatsu C11440 camera at 2048 × 2048 and 100 fps, the live preview shows the overshoot of a PI M-112.1DG stage move: the stage passes the target and moves back. We have not seen this in the live preview of other camera control software.
