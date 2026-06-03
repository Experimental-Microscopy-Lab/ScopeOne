# ScopeOne Python API

Minimal external Python API for controlling a running `ScopeOne.exe`, loading a Micro-Manager config, starting preview, recording frames into a session, and saving when needed.

## Project layout

- Python package project root: `ScopeOneCore/python/scopeone`
- Package source: `ScopeOneCore/python/scopeone/src/scopeone`
- Public facade: `scopeone.core`
- External backend client: `scopeone.client`
- Session wrapper: `scopeone.session`
- Shared-memory frame helpers: `scopeone.shm`

## Current API

```python
from scopeone import ScopeOne

scopeone = ScopeOne()
scopeone.load_config(r"C:\path\to\MMConfig.cfg")
print(scopeone.camera_ids())
scopeone.start_preview("Camera")

session = scopeone.record(frames=10, camera="Camera")
image = session.frame("Camera", 0)
paths = session.save(r"C:\data", base_name="test", format="tiff")

scopeone.stop_preview("Camera")
scopeone.unload_config()
```

## Available methods

- `ScopeOne.load_config(config_path)`
- `ScopeOne.unload_config()`
- `ScopeOne.camera_ids()`
- `ScopeOne.start_preview(camera="All")`
- `ScopeOne.stop_preview(camera="All")`
- `ScopeOne.record(frames, camera="All", timeout_ms=120000)`
- `RecordingSession.camera_ids()`
- `RecordingSession.frame_count(camera=None)`
- `RecordingSession.frame(camera, index)`
- `RecordingSession.frames(camera)`
- `RecordingSession.save(save_dir, base_name, format="tiff", compression=False, compression_level=6)`

## Notes

- `ScopeOne()` connects to a running `ScopeOne.exe` control server over the local Windows pipe `\\.\pipe\ScopeOne.Api.local`.
- `ScopeOne.connect("local")` is an alias for the default local connection.
- Start `ScopeOne.exe` before using the Python API.
- Frame reads use a shared-memory export block plus `scopeone.shm` parsing helpers.
- The package itself depends on `numpy` and `pywin32`. `Pillow` is only needed if you want notebook/image display helpers in your own code.

## Build

Build the desktop app when needed:

```powershell
.\scripts\build.ps1 --target all
```
