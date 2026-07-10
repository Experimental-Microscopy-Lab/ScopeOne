# ScopeOne Python API

Minimal external Python API for controlling a running ScopeOne app, loading a Micro-Manager config, starting preview, recording frames into a session, and saving when needed.

## Project layout

- Python package project root: `ScopeOneCore/python/scopeone`
- Package source: `ScopeOneCore/python/scopeone/src/scopeone`
- Public facade: `scopeone.core`
- External backend client: `scopeone.client`
- Session wrapper: `scopeone.session`
- Shared-memory frame helpers: `scopeone.shm`

## Current API

```python
import numpy as np

from scopeone import ScopeOne

scopeone = ScopeOne()
print(scopeone.version())
scopeone.load_config(r"C:\path\to\MMConfig.cfg")
print(scopeone.status())
print(scopeone.camera_ids())
print(scopeone.loaded_devices())
scopeone.start_preview("Camera")
live = scopeone.latest_raw_frame("Camera")
preview_image = np.flipud(live.image)
layer_key = scopeone.show_frame(live, image=preview_image, layer_id="python_live", name="Python Live")
scopeone.set_layer_layout("overlay")
scopeone.set_layer_display(layer_key, colormap="Fire", opacity_percent=80)
synthetic = np.zeros_like(live.image)
scopeone.show_image(synthetic, layer_id="python_synthetic", name="Python Synthetic")
markup_id = scopeone.create_rect_markup(layer_key, 10, 10, 100, 80, "Region")
scopeone.remove_markup(markup_id)

property_names = scopeone.device_property_names("Camera")
print(property_names[:10])

for prop in scopeone.device_properties("Camera")[:5]:
    print(prop["name"], prop["value"], prop["type"])

if "Exposure" in property_names:
    print(scopeone.get_property("Camera", "Exposure", from_cache=False))
    # scopeone.set_property("Camera", "Exposure", "10")

print(scopeone.read_exposure("Camera"))
# scopeone.set_exposure(10.0, "Camera")
# scopeone.set_roi("Camera", 0, 0, 256, 256)
# scopeone.clear_roi("Camera")

scopeone.set_processing_bit_depth(16)
module_index = scopeone.add_processing_module("gaussian_blur", {"kernel_size": 5, "sigma": 1.2})
print(scopeone.processing_modules())
scopeone.set_processing_module_parameters(module_index, {"kernel_size": 7})

session = scopeone.record(frames=10, camera="Camera")
stage = session.process_frame("Camera", 0, end_module_index=0)
edited_stage = np.clip(stage.image * 1.2, 0, (1 << stage.bits_per_sample) - 1)
result = scopeone.continue_pipeline(stage, image=edited_stage)
scopeone.show_frame(result, layer_id="python_result", name="Python Result")
paths = scopeone.save_frame(result, r"C:\data", base_name="python_result", format="tiff")
session_paths = session.save(r"C:\data", base_name="raw_session", format="tiff")
session.close()

scopeone.stop_preview("Camera")
scopeone.unload_config()
scopeone.close()
```

## Available methods

- `ScopeOne.load_config(config_path)`
- `ScopeOne.unload_config()`
- `ScopeOne.close()`
- `ScopeOne.version()`
- `ScopeOne.status()`
- `ScopeOne.camera_ids()`
- `ScopeOne.loaded_devices()`
- `ScopeOne.start_preview(camera="All")`
- `ScopeOne.stop_preview(camera="All")`
- `ScopeOne.list_layers()`
- `ScopeOne.layer_options()`
- `ScopeOne.set_layer_layout(layout)`
- `ScopeOne.set_selected_layers(layer_keys)`
- `ScopeOne.set_layer_display(layer_key, visible=None, opacity_percent=None, gamma=None, colormap=None, blending=None, levels=None)`
- `ScopeOne.move_layer(layer_key, offset)`
- `ScopeOne.config_groups()`
- `ScopeOne.configs(group)`
- `ScopeOne.current_config(group)`
- `ScopeOne.set_config(group, config)`
- `ScopeOne.remove_static_layer(layer_key)`
- `ScopeOne.clear_static_layers()`
- `ScopeOne.create_line_markup(layer_key, x1, y1, x2, y2, label="")`
- `ScopeOne.create_rect_markup(layer_key, x, y, width, height, label="")`
- `ScopeOne.list_markups(layer_key=None)`
- `ScopeOne.remove_markup(markup_id)`
- `ScopeOne.clear_markups(layer_key=None)`
- `ScopeOne.device_properties(device, from_cache=True)`
- `ScopeOne.device_property_names(device)`
- `ScopeOne.get_property(device, property, from_cache=True)`
- `ScopeOne.set_property(device, property, value)`
- `ScopeOne.read_exposure(camera="All")`
- `ScopeOne.set_exposure(exposure_ms, camera="All")`
- `ScopeOne.get_roi(camera)`
- `ScopeOne.set_roi(camera, x, y, width, height)`
- `ScopeOne.clear_roi(camera)`
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
- `ScopeOne.processing_state()`
- `ScopeOne.processing_modules()`
- `ScopeOne.set_processing_bit_depth(bit_depth)`
- `ScopeOne.set_realtime_processing(enabled)`
- `ScopeOne.start_processing()`
- `ScopeOne.stop_processing()`
- `ScopeOne.add_processing_module(kind, parameters=None)`
- `ScopeOne.remove_processing_module(index)`
- `ScopeOne.set_processing_module_parameters(index, parameters)`
- `ScopeOne.reset_processing_module_state(index)`
- `ScopeOne.frame_mapping_info()`
- `ScopeOne.write_frame_mapping(image, camera="python", bits_per_sample=None, frame_index=0, source_roi=None)`
- `ScopeOne.process_frame_mapping(camera=None, start_module_index=None, end_module_index=None)`
- `ScopeOne.process_image(image, camera="python", bits_per_sample=None, start_module_index=None, end_module_index=None)`
- `ScopeOne.latest_raw_frame(camera)`
- `ScopeOne.show_frame_mapping_as_layer(layer_id="python_result", name="Python Result", camera=None)`
- `ScopeOne.save_frame_mapping(save_dir, base_name, format="tiff", compression=False, compression_level=6, camera=None)`
- `ScopeOne.continue_pipeline(frame, image=None)`
- `ScopeOne.show_frame(frame, image=None, layer_id="python_result", name="Python Result")`
- `ScopeOne.show_image(image, layer_id="python_result", name="Python Result", camera="python", bits_per_sample=None)`
- `ScopeOne.save_frame(frame, save_dir, base_name, image=None, format="tiff", compression=False, compression_level=6)`
- `ScopeOne.save_image(image, save_dir, base_name, format="tiff", compression=False, compression_level=6, camera="python", bits_per_sample=None)`
- `ScopeOne.record(frames, camera="All", timeout_ms=120000, mda_interval_ms=0.0, z_positions=None, positions=None, order=None)`
- `RecordingSession.camera_ids()`
- `RecordingSession.frame_count(camera=None)`
- `RecordingSession.frame(camera, index)`
- `RecordingSession.process_frame(camera, index, start_module_index=None, end_module_index=None)`
- `RecordingSession.frames(camera)`
- `RecordingSession.save(save_dir, base_name, format="tiff", compression=False, compression_level=6)`
- `RecordingSession.close()`
- `FrameResult.write(image=None)`

`FrameResult` exposes common metadata as typed attributes: `camera`, `width`, `height`, `pixel_format`, `bits_per_sample`, `frame_index`, `timestamp_ns`, `source_roi`, `module_index`, `next_module_index`, and `start_module_index`.

`FrameResult.write(image)` clips and casts the edited pixels to the frame format, writes them into the shared mapping, and updates `frame.image` to the written array.

## Local API Protocol

ScopeOne uses one local control pipe for JSON commands and one shared-memory block for frame transfer.

- Control endpoint on Windows: `\\.\pipe\ScopeOne.Api.local`
- Control endpoint on Linux/Unix: `<tempdir>/ScopeOne.Api.local`
- Message framing: 4-byte little-endian unsigned payload size, followed by UTF-8 JSON.
- Maximum JSON payload: 256 KiB.
- Success response: `{"type": "<request type>", "ok": true, ...}`
- Error response: `{"type": "<request type>", "ok": false, "error": "..."}`

### Control requests

- `ping`: health check.
- `version`: response `version`, `apiVersion`.
- `status`: response `version`, `apiVersion`, cameras, devices, running previews, processing state, and layer keys.
- `frame_mapping_info`: response `mappingName`, `mappingSize`, `headerBytes`, `maxPayloadBytes`, and supported `pixelFormats`.
- `load_config`: fields `configPath`; response `cameraIds`.
- `unload_config`: unload current Micro-Manager config.
- `camera_ids`: response `cameraIds`.
- `loaded_devices`: response `devices`.
- `start_preview`: fields `camera`, accepts a camera id or `"All"`.
- `stop_preview`: fields `camera`, accepts a camera id or `"All"`.
- `list_layers`: response `layers`.
- `layer_options`: response `layouts`, `colormaps`, `blendingModes`.
- `set_layer_layout`: fields `layout`, accepts `side_by_side` or `overlay`.
- `set_selected_layers`: fields `layerKeys`; response `selectedLayers`.
- `set_layer_display`: fields `layerKey`, optional `visible`, `opacityPercent`, `gamma`, `colormap`, `blending`, and display level fields `minLevel`, `maxLevel`, `maxPossible`.
- `move_layer`: fields `layerKey`, `offset`; response ordered `layers`.
- `config_groups`: response `groups`.
- `configs`: fields `group`; response `configs`.
- `current_config`: fields `group`; response `config`.
- `set_config`: fields `group`, `config`; response `config`.
- `remove_static_layer`: fields `layerKey`.
- `clear_static_layers`: remove all static preview layers.
- `create_line_markup`: fields `layerKey`, `x1`, `y1`, `x2`, `y2`, optional `label`; response `markupId`.
- `create_rect_markup`: fields `layerKey`, `x`, `y`, `width`, `height`, optional `label`; response `markupId`.
- `list_markups`: optional field `layerKey`; response `markups` with `id`, `type`, `role`, `layerKey`, `label`, and geometry fields.
- `remove_markup`: fields `markupId`.
- `clear_markups`: optional field `layerKey`.
- `device_property_names`: fields `device`; response `names`.
- `device_properties`: fields `device`, `fromCache`; response `properties`.
- `get_property`: fields `device`, `property`, `fromCache`; response `value`.
- `set_property`: fields `device`, `property`, `value`.
- `read_exposure`: optional field `camera`; response `exposureMs`.
- `set_exposure`: fields `exposureMs`, optional field `camera`; response may include the read-back `exposureMs`.
- `get_roi`: fields `camera`; response `x`, `y`, `width`, `height`.
- `set_roi`: fields `camera`, `x`, `y`, `width`, `height`; response may include read-back ROI geometry.
- `clear_roi`: fields `camera`.
- `xy_stage_devices`: response `devices`.
- `z_stage_devices`: response `devices`.
- `current_xy_stage_device`: response `device`.
- `current_focus_device`: response `device`.
- `read_xy_position`: optional field `device`; response `x`, `y`.
- `read_z_position`: optional field `device`; response `z`.
- `move_xy_relative`: fields `device`, `dx`, `dy`.
- `move_z_relative`: fields `device`, `dz`.
- `move_xy_to`: fields `device`, `x`, `y`.
- `move_z_to`: fields `device`, `z`.
- `processing_modules`: response `bitDepth`, `realTime`, and `modules`.
- `set_processing_bit_depth`: fields `bitDepth`, accepts `8` or `16`.
- `set_realtime_processing`: fields `enabled`.
- `add_processing_module`: fields `kind`, optional `parameters`; response `index`.
- `remove_processing_module`: fields `index`.
- `set_processing_module_parameters`: fields `index`, `parameters`.
- `reset_processing_module_state`: fields `index`.
- `record`: fields `frames`, `camera`, `timeoutMs`, `mdaIntervalMs`, `zPositions`, `positions`, `order`; response `sessionId`, `cameraIds`.
- `session_info`: fields `sessionId`; response `cameraIds`, `frameCount`, `frameCounts`.
- `session_close`: fields `sessionId`; releases the recorded session held by the app.
- `session_frame`: fields `sessionId`, `camera`, `index`; response `mappingName`, `mappingSize`, and frame metadata.
- `latest_raw_frame`: fields `camera`; response `mappingName`, `mappingSize`, and frame metadata.
- `session_process_frame`: fields `sessionId`, `camera`, `index`, optional `startModuleIndex` or `endModuleIndex`; response `mappingName`, `mappingSize`, frame metadata, and optional stage metadata.
- `process_frame_mapping`: optional fields `camera`, `startModuleIndex` or `endModuleIndex`; response `mappingName`, `mappingSize`, frame metadata, and optional stage metadata.
- `show_frame_mapping_as_layer`: optional fields `camera`, `layerId`, `name`; imports the current shared memory frame as a preview layer and returns `layerKey`.
- `save_frame_mapping`: fields `saveDir`, `baseName`, `format`, `compression`, `compressionLevel`, optional field `camera`; imports the current shared memory frame and saves it as a one-frame output.
- `session_save`: fields `sessionId`, `saveDir`, `baseName`, `format`, `compression`, `compressionLevel`; response `paths`.

### Record request

```json
{
  "type": "record",
  "frames": 1,
  "camera": "Camera",
  "timeoutMs": 120000,
  "mdaIntervalMs": 0.0,
  "zPositions": [0.0, 1.0],
  "positions": [[0.0, 0.0]],
  "order": ["time", "z", "xy"]
}
```

`record` returns `sessionId` and `cameraIds`. If `zPositions` or `positions` is non-empty, recording uses the MDA snap path. If both are empty, recording uses the preview/raw-frame path.

Processing module editing follows the desktop UI rules: stop real-time processing before changing bit depth, adding/removing modules, updating module parameters, or resetting module state. `add_processing_module` accepts `fft`, `background_calibration`, `spatiotemporal_binning`, `gaussian_blur`, and `differential_rolling`.

### Frame transfer

`session_frame` writes the selected frame into shared memory:

- Mapping name: `ScopeOne.Api.frame`
- Header layout: `scopeone::core::SharedFrameHeader`
- Pixel data starts at `scopeone::core::kSharedFrameHeaderSize`
- Python reads this through `scopeone.shm.frame_to_ndarray()`
- Responses include `camera`, `width`, `height`, `stride`, string `payloadBytes`, `pixelFormat`, `bitsPerSample`, string `frameIndex`, string `timestampNs`, `sourceRoiX`, `sourceRoiY`, `sourceRoiWidth`, `sourceRoiHeight`, and `sourceRoiValid`.
- Session frames can come from memory, saved TIFF stacks, or saved binary streams.
- `ScopeOne.latest_raw_frame(...)`, `RecordingSession.frame(...)`, `RecordingSession.frames(...)`, `RecordingSession.process_frame(...)`, and `ScopeOne.process_frame_mapping(...)` return `FrameResult` objects.

`latest_raw_frame` exports the current live raw frame for Python processing. `session_process_frame` reads a stored session frame, processes it through the current pipeline, and writes the result into the same shared memory block. Use `endModuleIndex` to stop after one stage. The response then includes `moduleIndex` and `nextModuleIndex`. Pass the edited numpy array to `ScopeOne.continue_pipeline(frame, image=edited_image)` to write it back and continue with the next pipeline stage. Pass a frame and optional edited image to `ScopeOne.show_frame(frame, image=edited_image)` to display it in the ScopeOne preview as a static layer. Use `ScopeOne.save_frame(frame, save_dir, base_name, image=edited_image)` to save the current Python/C++ result directly. `FrameResult.write()` requires the shared mapping to still contain the same frame metadata, so request the frame again after another frame export overwrites the mapping. `process_frame_mapping`, `show_frame_mapping_as_layer`, and `save_frame_mapping` reuse the last exported or explicitly imported camera id when `camera` is omitted. Provide `camera` the first time a mapping was written by an external client.

Python can also publish a new numpy image without first requesting a ScopeOne frame. `ScopeOne.write_frame_mapping(...)` writes a 2D mono array into the shared frame mapping and returns a `FrameResult`; `ScopeOne.show_image(...)`, `ScopeOne.process_image(...)`, and `ScopeOne.save_image(...)` are convenience wrappers around that path.

### Error policy

The server returns `ok: false` and a short actionable `error` string. Recording errors distinguish startup failure, timeout before session data, MDA captured no frames, preview-frame recording captured no frames, and invalid session/frame indexes.

## Notes

- `ScopeOne()` connects to a running ScopeOne control server over the local platform endpoint.
- `ScopeOne.connect("local")` is an alias for the default local connection.
- Start the ScopeOne app before using the Python API.
- Python 3.10 or newer is required.
- Frame reads use a shared-memory export block plus `scopeone.shm` parsing helpers.
- The package itself depends on `numpy` and `pywin32`. `Pillow` is only needed if you want notebook/image display helpers in your own code.
- Runnable examples are kept in `examples/example_minimal.ipynb`.

## Build

Build the desktop app when needed:

```powershell
.\scripts\build.ps1 --target all
```
