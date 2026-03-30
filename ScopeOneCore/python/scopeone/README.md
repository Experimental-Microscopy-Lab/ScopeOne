# ScopeOne Python API

Minimal Python package for loading a Micro-Manager config, recording frames into a session, and saving when needed.

## Project layout

- Python package project root: `ScopeOneCore/python/scopeone`
- Package source: `ScopeOneCore/python/scopeone/src/scopeone`
- Pybind C++ binding source: `ScopeOneCore/python/ScopeOneCoreBindings.cpp`

## Current API

```python
from scopeone import ScopeOne

scopeone = ScopeOne()
scopeone.load(r"C:\path\to\MMConfig.cfg")
print(scopeone.camera_ids())

session = scopeone.record(frame_count=10, camera="Camera")
image = session.frame("Camera", 0)
paths = session.save(r"C:\data", base_name="test", format="tiff")

scopeone.unload()
```

## Available methods

- `ScopeOne.load(config_path)`
- `ScopeOne.unload()`
- `ScopeOne.camera_ids()`
- `ScopeOne.record(frame_count, camera="All", timeout_ms=120000)`
- `RecordingSession.camera_ids()`
- `RecordingSession.frame_count(camera=None)`
- `RecordingSession.frame(camera, index)`
- `RecordingSession.frames(camera)`
- `RecordingSession.save(save_dir, base_name, format="tiff", compression=False, compression_level=6)`

## Build

Build the Python extension when needed:

```powershell
cmake --build build --config Release --parallel --target scopeone_core
```

The target is defined directly in `ScopeOneCore/CMakeLists.txt`.
The extension is emitted into `ScopeOneCore/python/scopeone/src/scopeone` as `scopeone._core`.
