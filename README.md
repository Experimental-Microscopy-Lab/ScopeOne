<p align="center">
  <img src="resources/Scopeone_Logo.svg" width="400">
</p>


ScopeOne is an open-source microscopy control software for multi-camera imaging, originally developed for in-house lab use. Built with C++ and Qt, it uses a multi-process architecture where each camera runs in its own [MMCore](https://github.com/micro-manager/mmCoreAndDevices) instance, enabling simultaneous preview and acquisition across multiple cameras.
It retains full compatibility with the [Micro-Manager](https://micro-manager.org/) device ecosystem and adds a modular real-time image processing pipeline with support for background calibration, temporal filtering, FFT analysis, and more.

<p align="center">
  <img src="resources/MainWindow.png" width="720"><br>
  <sub> Graphical User Interface of ScopeOne</sub>
</p>

As an open-source project, ScopeOne builds on existing community efforts to reduce duplicated work and provides an alternative that enriches the microscopy community. While the current development is conducted in close collaboration with the optics and biology teams within our laboratory, we aim to expand engagement with the broader research community to make the platform more practical, accessible, and universal. Any issues or pull requests are greatly appreciated！

## Quick Start

### For Users

Download the latest release package from the [Releases](https://github.com/Experimental-Microscopy-Lab/ScopeOne/releases) page and extract it. Run `ScopeOne.exe` to start the application.

**System Requirements:**
- Windows 10/11 (64-bit)
- Micro-Manager device adapters for your hardware

**Device Adapter Setup:**

ScopeOne loads Micro-Manager configuration files (.cfg) directly. We recommend installing [Micro-Manager 2.0](https://download.micro-manager.org/nightly/2.0/Windows/) to access the full device adapter library. The release package includes only a minimal set of device adapter DLLs. To add support for additional devices, simply copy the required DLLs(`mmgr_dal_xxx.dll`) from your Micro-Manager installation directory (typically `C:\Program Files\Micro-Manager-2.0`) to the root folder containing `ScopeOne.exe`. Besides, we kindly remind you first ensure your devices are working properly in Micro-Manager before using them in ScopeOne, as device compatibility issues are often related to the device adapter itself.

**Dual-camera Setup:**

There is an example dual-camera .cfg file in the config folder, just change the camera labels in Devices section to match your camera models.

### For Developers

**Prerequisites:**
- [CMake](https://cmake.org/download/) 4.1.0
- [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) (MSVC v143 toolset)
- [Qt](https://www.qt.io/development/download-qt-installer-oss) 6.9.1 (msvc2022_64)
- OpenCV 4.12.0
- libtiff 4.7.1,
- zlib 1.3.1
- mmCoreAndDevices

Extract the bundled third-party dependencies into `ScopeOneCore/external` under this repository. Expected third-party dependencies path layout:

```text
ScopeOne/
  ScopeOneCore/
    external/
      mmCoreAndDevices/
      opencv-4.12.0/
      pybind11-3.0.1/
      tiff-4.7.1/
      zlib-1.3.1/
```

<!-- Setting up these dependencies can be time-consuming. To simplify this, we provide a pre-packaged development source archive that includes third-party libraries (OpenCV, MMCore, libtiff, zlib and pybind11) except Qt, VS and CMake. Download the development package from [Releases](https://github.com/Experimental-Microscopy-Lab/ScopeOne/releases). -->

**Windows Build Steps:**

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

**Linux Build Steps (experimental):**

On Linux, build Micro-Manager from the top-level `micro-manager` repository, then create a symlink so ScopeOne can find the `mmCoreAndDevices` tree at the path expected by the current CMake files.

1. Install common build dependencies:
```bash
sudo apt install \
  git subversion build-essential cmake autoconf automake libtool autoconf-archive \
  pkg-config swig libboost-all-dev qt6-base-dev libopencv-dev libtiff-dev zlib1g-dev
```

2. Clone Micro-Manager and create the `mmCoreAndDevices` symlink:
```bash
cd /path/to/ScopeOne/ScopeOneCore
mkdir -p external
cd external

git clone --recurse-submodules https://github.com/micro-manager/micro-manager.git micro-manager
ln -s micro-manager/mmCoreAndDevices mmCoreAndDevices

cd micro-manager
git submodule update --init --recursive
```

3. Build Micro-Manager without the Java application layer:
```bash
./autogen.sh
./configure --without-java --enable-static
make fetchdeps
make -j"$(nproc)"
```

ScopeOne currently links Linux MMCore from:

```text
ScopeOneCore/external/mmCoreAndDevices/MMCore/.libs/libMMCore.a
```

4. Build and install `ScopeOneCore`:
```bash
cd /path/to/ScopeOne
cmake -S ScopeOneCore -B ScopeOneCore/build -DCMAKE_BUILD_TYPE=Release
cmake --build ScopeOneCore/build --parallel
cmake --install ScopeOneCore/build
```

5. Build the GUI application:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The Linux executable is expected at:

```text
build/ScopeOne
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
