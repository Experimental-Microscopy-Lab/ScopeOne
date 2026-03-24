# ScopeOne

## Overview

ScopeOne is a high-performance microscopy control software built with C++ and Qt6, leveraging Micro-Manager Core (MMCore) for hardware abstraction. It addresses specific needs in multi-camera imaging while maintaining compatibility with the extensive Micro-Manager device ecosystem. It originated as our lab software and tranition to an open-source project.

The software implements a multi-process architecture that enables true simultaneous preview and acquisition from multiple cameras. Each camera runs in its own MMCore instance, real-time image processing is built into the core with a modular pipeline supporting background calibration, temporal filtering, FFT analysis, and other operations. 

## Quick Start

### For Users

Download the latest release package from the [Releases](https://github.com/yourusername/ScopeOne/releases) page and extract it. Run `ScopeOne.exe` to start the application.

**System Requirements:**
- Windows 10/11 (64-bit)
- Micro-Manager device adapters for your hardware

**Device Adapter Setup:**

ScopeOne loads Micro-Manager configuration files (.cfg) directly. We recommend installing Micro-Manager 2.0 to access the full device adapter library. The release package includes only a minimal set of device adapter DLLs. To add support for additional devices, simply copy the required DLL files from your Micro-Manager installation directory (typically `C:\Program Files\Micro-Manager-2.0`) to the folder containing `ScopeOne.exe`.

### For Developers

**Prerequisites:**
- CMake 3.16+
- Visual Studio 2022 (MSVC v143 toolset)
- Qt 6.9.1 (msvc2022_64)
- OpenCV 4.10.0
- libtiff 4.7.1,
- zlib 1.3.1
- mmCoreAndDevices

**Quick Setup:**

Setting up a C++ development environment can be time-consuming. To simplify this, we provide a pre-packaged development source archive that includes all third-party libraries (OpenCV, MMCore, libtiff, zlib, pybind11) except Qt, VS and CMake. Download the development package from [Releases](https://github.com/yourusername/ScopeOne/releases).

**Build Steps:**

1. Build and install `ScopeOneCore`:
```powershell
cmake -S ScopeOneCore -B ScopeOneCore/build
cmake --build ScopeOneCore/build --config Release --parallel
cmake --install ScopeOneCore/build --config Release
```

2. Build the GUI application:
```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
```

3. Run:
```powershell
.\build\Release\ScopeOne.exe
```

4. Create distribution package:
```powershell
cmake --build build --config Release --target package
```



## Tested Devices
- Yokogawa CSU X1
- Hamamatsu C13440
- Andor 897D

The current validation list is still short, but the codebase has been cleaned to remove early hard-coded device assumptions. In principle, ScopeOne should follow Micro-Manager device compatibility.

## Tested System Configurations
- Windows 10, Dual Intel(R) Xeon(R) E5-2637 v3, 64 GB RAM, NVIDIA Quadro K620  
- Windows 11, Intel(R) Core(TM) Ultra 5 125U, 64 GB RAM

Although these machines are older and weaker than many typical lab computers, ScopeOne still provides smooth real-time preview and processing on them.

