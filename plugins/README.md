# Extending ScopeOne

ScopeOne has two public extension points. Both run outside the ScopeOne process, so a crash in an
extension does not take ScopeOne down.

| Extension | You write | Runs in | Example |
|---|---|---|---|
| Hardware plugin | a `DriverHostProviderPlugin` in C++ against `scopeone::HardwarePluginSDK` | `ScopeOne_DriverHost`, one process per device | `hardware/ExampleCamera` |
| Python processing module | a decorated Python function | its own Python process | `ScopeOneCore/python/scopeone/examples/processing_offset.py` |

The CUDA and ONNX processing modules in `ScopeOneCuda` and `ScopeOneInference` are built-in plugins,
built and released with ScopeOne, because they link CUDA and ONNX Runtime. Their interface,
`scopeone/ProcessingPlugin.h`, changes with ScopeOne and is not meant for code outside this
repository. Everything else, including the tool windows and the DAQ and signal source devices, is
part of ScopeOne itself.

## Hardware plugins

A hardware plugin implements `DriverHostProviderPlugin` and returns a `HardwareProvider` with the
capabilities its devices have (camera, stage, shutter, state, configuration). The headers it may
use are the ones `scopeone/HardwarePluginSDK.h` includes. ScopeOne starts one `ScopeOne_DriverHost`
for each device, the host loads the plugin library, and from then on ScopeOne only exchanges
messages and frames with the host.

Build and install ScopeOneCore first (`scripts/build-unix.sh` or `scripts\build.ps1`). A hardware
plugin is then a separate CMake project:

```cmake
cmake_minimum_required(VERSION 3.23)
project(MyCamera LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(ScopeOneCore CONFIG REQUIRED)

add_library(MyCamera MODULE MyCamera.cpp plugin.json)
target_link_libraries(MyCamera PRIVATE scopeone::HardwarePluginSDK Qt6::Core)
scopeone_add_plugin(MyCamera hardware)
```

Point `CMAKE_PREFIX_PATH` at `ScopeOneCore/install` and at the same Qt that ScopeOne uses
(`vcpkg_installed/<triplet>`).

`plugin.json` sits beside `CMakeLists.txt`:

```json
{
  "id": "my.camera",
  "name": "My Camera",
  "version": "1.0.0",
  "scopeOneApi": 1,
  "kind": "hardware",
  "providerId": "my.camera",
  "autoLoad": false
}
```

`scopeOneApi` is `ScopeOneHardwarePluginApiVersion` in `scopeone/PluginManifest.h`.
`scopeone_add_plugin` writes `scopeone_plugin.json` from it with the Core version added, and the
plugin names that file:

```cpp
Q_PLUGIN_METADATA(IID ScopeOneDriverHostProviderPlugin_iid FILE "scopeone_plugin.json")
```

The plugin library is loaded by `ScopeOne_DriverHost`, which links the same Core and Qt as
ScopeOne, so a hardware plugin is built with the same compiler, Qt and Core release
(`MAJOR.MINOR` of `VERSION`). ScopeOne refuses a plugin built against another release and names
the release it needs.

Copy the built library into `plugins/hardware/` beside the ScopeOne executable, or install it with
**Tools > Plugin Manager**, which puts it in the user's application data directory. Hardware
plugins are enabled and configured in the Plugin Manager and take effect after a restart.

## Python processing modules

```python
import numpy as np
from scopeone.processing import Integer, Real, Boolean, Choice, module

@module("my.python.threshold", "My Threshold", Integer("level", "Level", 100, 0, 65535))
def threshold(image, level, bits):
    return np.where(image > level, (1 << bits) - 1, 0).astype(image.dtype)
```

The function gets a 2D `uint8` or `uint16` array and the parameters by name, and returns a 2D
`uint8` or `uint16` array. A function with a `bits` argument also gets the bits per sample of
the frame, 12 for a 12-bit camera in a `uint16` array, so `bits` is not a parameter key. Put
the script in `plugins/processing/python/` beside ScopeOne or in the same folder of the user's
plugin directory. The modules appear in the pipeline after a restart. ScopeOne ships no script
there; `ScopeOneCore/python/scopeone/examples/processing_offset.py` is one to copy.

Each module in a pipeline runs in its own Python process (`python -m scopeone.processing
serve script.py`). That interpreter needs numpy and the `scopeone` package
(`pip install ScopeOneCore/python/scopeone`). Choose it under **Settings > Python
Interpreter**, or with `SCOPEONE_PYTHON`, which takes precedence; otherwise ScopeOne runs
`python3` (`python` on Windows). Output printed by a module goes to the log, and an exception
in it shows as the error of that pipeline step.

ScopeOne first runs `python -m scopeone.processing describe script.py`, which prints one JSON
line with the modules of the script and their parameters. While the pipeline runs, each frame is
one request on stdin: a JSON line with `module`, `parameters`, `width`, `height`, `stride`,
`format` (`mono8` or `mono16`), `bits` and `bytes`, followed by that many pixel bytes. The reply
on stdout has the same form with `width`, `height`, `format` and `bytes`, or a JSON line with
`error`.
