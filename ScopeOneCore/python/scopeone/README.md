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

property_names = scopeone.device_property_names("Camera")
print(property_names[:10])

for prop in scopeone.device_properties("Camera")[:5]:
    print(prop["name"], prop["value"], prop["type"])

if "Exposure" in property_names:
    print(scopeone.get_property("Camera", "Exposure", from_cache=False))
    # scopeone.set_property("Camera", "Exposure", "10")

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
- `ScopeOne.device_properties(device, from_cache=True)`
- `ScopeOne.device_property_names(device)`
- `ScopeOne.get_property(device, property, from_cache=True)`
- `ScopeOne.set_property(device, property, value)`
- `ScopeOne.xy_stage_devices()`
- `ScopeOne.z_stage_devices()`
- `ScopeOne.current_xy_stage_device()`
- `ScopeOne.current_focus_device()`
- `ScopeOne.read_xy_position(device=None)`
- `ScopeOne.read_z_position(device=None)`
- `ScopeOne.move_xy_relative(dx, dy, device=None)`
- `ScopeOne.move_z_relative(dz, device=None)`
- `ScopeOne.move_xy_to(x, y, device=None)`
- `ScopeOne.move_z_to(z, device=None)`
- `ScopeOne.record(frames, camera="All", timeout_ms=120000, mda_interval_ms=0.0, z_positions=None, positions=None, order=None)`
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
- Runnable examples are kept in `examples/example_minimal.ipynb`.

## Build

Build the desktop app when needed:

```powershell
.\scripts\build.ps1 --target all
```
