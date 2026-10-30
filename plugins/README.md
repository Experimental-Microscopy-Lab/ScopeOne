# Writing ScopeOne plugins

ScopeOne loads four kinds of extensions:

| Kind | Contract | Runs in | Example |
|---|---|---|---|
| Processing | `ProcessingPlugin` and `ProcessingModule` in `scopeone/ProcessingPlugin.h` | ScopeOne | `processing/ExampleProcessing` |
| Python processing | a decorated Python function, see below | its own Python process | `processing/PythonProcessing/python` |
| Tool | `ScopeOneToolPlugin` in `scopeone/ToolPlugin.h` | ScopeOne, as a window | `tools/ExampleTool` |
| Hardware | `DriverHostProviderPlugin`, `DaqDevicePlugin`, `SignalSourcePlugin` | `ScopeOne_DriverHost`, one process per device | `hardware/ExampleHardware` |

A processing module declares its parameters in a `ProcessingModuleDescriptor`. The pipeline
panel, the Local API and MCP build their controls from it, so a module needs no UI code.

A tool gets a `ScopeOneToolContext`. Its `core()` is a `ScopeOneToolCore`: the layers and their
images, signal sources and DAQ devices. Tools use only this part of the Core.

## Building a C++ plugin

Build and install ScopeOneCore first (`scripts/build-unix.sh` or the Windows script). A plugin
is then a separate CMake project:

```cmake
cmake_minimum_required(VERSION 3.23)
project(MyPlugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(ScopeOneCore CONFIG REQUIRED)

add_library(MyPlugin MODULE MyPlugin.cpp plugin.json)
target_link_libraries(MyPlugin PRIVATE scopeone::PluginSDK Qt6::Core)
scopeone_add_plugin(MyPlugin processing)   # processing, tools or hardware
```

Point `CMAKE_PREFIX_PATH` at `ScopeOneCore/install` and at the same Qt that ScopeOne uses
(`vcpkg_installed/<triplet>`).

`plugin.json` sits beside `CMakeLists.txt`:

```json
{
  "id": "my.plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "scopeOneApi": 2,
  "kind": "processing"
}
```

`scopeOneApi` is the contract version of the kind (`ScopeOne*PluginApiVersion` in
`scopeone/PluginManifest.h`). `scopeone_add_plugin` writes `scopeone_plugin.json` from it with
the Core version added, and the plugin names that file:

```cpp
Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "scopeone_plugin.json")
```

Plugins share C++ types with ScopeOne, so they must be built with the same compiler, the same
Qt and the same Core release (`MAJOR.MINOR` of `VERSION`). ScopeOne refuses a plugin built
against another release and names the release it needs. Change `VERSION` when a public header
of the SDK changes.

## Installing

Copy the built library into `plugins/<kind>/` beside the ScopeOne executable, or install it
with **Tools > Plugin Manager**, which puts it in the user's application data directory.
Hardware plugins are enabled and configured in the Plugin Manager and take effect after a
restart.

## Python processing modules

```python
import numpy as np
from scopeone.processing import Integer, Real, Boolean, Choice, module

@module("my.python.threshold", "My Threshold", Integer("level", "Level", 100, 0, 65535))
def threshold(image, level):
    return np.where(image > level, np.iinfo(image.dtype).max, 0).astype(image.dtype)
```

The function gets a 2D `uint8` or `uint16` array and the parameters by name, and returns a 2D
`uint8` or `uint16` array. Put the script in `plugins/processing/python/` beside ScopeOne or
in the same folder of the user's plugin directory. The modules appear in the pipeline after a
restart.

Each module in a pipeline runs in its own Python process (`python -m scopeone.processing
serve script.py`). That interpreter needs numpy and the `scopeone` package
(`pip install ScopeOneCore/python/scopeone`). Choose it under **Settings > Python
Interpreter**, or with `SCOPEONE_PYTHON`, which takes precedence; otherwise ScopeOne runs
`python3` (`python` on Windows). Output printed by a module goes to the log, and an exception
in it shows as the error of that pipeline step.
