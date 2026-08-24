# ScopeOne Python API

Python client for ScopeOne's language-neutral Local API. It controls a running ScopeOne app and can be used by scripts, notebooks, or Python-based agent tool adapters.

## Project layout

- Python package project root: `ScopeOneCore/python/scopeone`
- Package source: `ScopeOneCore/python/scopeone/src/scopeone`
- Public facade: `scopeone.core`
- External backend client: `scopeone.client`
- Session wrapper: `scopeone.session`
- Shared-memory frame helpers: `scopeone.shm`

## Current API

```python
import time

import numpy as np

from scopeone import ScopeOne

scopeone = ScopeOne()
print(scopeone.version())
scopeone.load_config(r"C:\path\to\MMConfig.cfg")
print(scopeone.status())
print(scopeone.capabilities())
print(scopeone.state_snapshot())
print(scopeone.camera_ids())
print(scopeone.loaded_devices())
scopeone.start_preview("Camera")
live = scopeone.latest_raw_frame("Camera")
preview_image = np.flipud(live.image)
layer_key = scopeone.show_frame(live, image=preview_image, layer_id="python_live", name="Python Live")
scopeone.set_layer_layout("overlay")
scopeone.set_layer_display(layer_key, colormap="Fire", opacity_percent=80)
scopeone.auto_layer_levels(layer_key)
print(scopeone.get_layer_histogram(layer_key)["mean"])
print(scopeone.get_line_profile(layer_key, 0, 0, 100, 100))
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

document = scopeone.experiment_document()
document["plan"]["framesPerBurst"] = 10
document = scopeone.validate_experiment(document)
scopeone.save_experiment(r"C:\data\experiment.scopeone.json", document)
experiment = scopeone.start_experiment(document)
while experiment.status()["state"] == "Running":
    time.sleep(0.05)
print(experiment.experiment_id(), experiment.status())
experiment.close()

session = scopeone.record(frames=10, camera="Camera")
stage = session.process_frame("Camera", 0, end_module_index=0)
edited_stage = np.clip(stage.image * 1.2, 0, (1 << stage.bits_per_sample) - 1)
result = scopeone.continue_pipeline(stage, image=edited_stage)
scopeone.show_frame(result, layer_id="python_result", name="Python Result")
paths = scopeone.save_frame(result, r"C:\data", base_name="python_result", format="ome-tiff")
session_paths = session.save(r"C:\data", base_name="raw_session", format="ome-tiff")
session.close()

scopeone.stop_preview("Camera")
scopeone.unload_config()
scopeone.close()
```

## Agent adapters

The standalone C++ `ScopeOneMcpServer` included with the desktop app is the default integration for MCP-compatible agents. It does not use this Python package. This package remains available for scripts, notebooks, and custom Python-based adapters that intentionally use the Local API directly.

`capabilities()` returns `operationGroups`, `longRunningOperations`, `hardwareMutationOperations`, `filesystemMutationOperations`, and `destructiveOperations`. `state_snapshot()` returns application and configuration identity, hardware inventory, preview state, processing modules, image layers, markups, live acquisition and writer progress, retained experiments, and recording sessions. It intentionally does not poll every hardware property; use `get_property(..., from_cache=False)`, position reads, exposure reads, or ROI reads when an action requires a fresh hardware value.

The Python client assigns a numeric `requestId` to every control request. The desktop app echoes it, and the client rejects a mismatched response. Raw protocol clients may provide their own string or numeric `requestId`.

A control connection is synchronous and processes one request at a time. Agent adapters should serialize calls made through one `ScopeOne` instance, especially frame operations because all clients share the same exported frame mapping.

## Available methods

- `ScopeOne.load_config(config_path)`
- `ScopeOne.unload_config()`
- `ScopeOne.close()`
- `ScopeOne.version()`
- `ScopeOne.status()`
- `ScopeOne.capabilities()`
- `ScopeOne.state_snapshot()`
- `ScopeOne.camera_ids()`
- `ScopeOne.loaded_devices()`
- `ScopeOne.start_preview(camera="All")`
- `ScopeOne.stop_preview(camera="All")`
- `ScopeOne.image_windows()`
- `ScopeOne.open_image_window(session_id, title=None, camera_id=None)`
- `ScopeOne.activate_image_window(document_id)`
- `ScopeOne.close_image_window(document_id=None)`
- `ScopeOne.process_image_window(document_id=None, complete_stack=False)`
- `ScopeOne.save_image_window(save_dir, base_name, document_id=None, format="ome-tiff", compression=False, compression_level=6)`
- `ScopeOne.list_layers()`
- `ScopeOne.get_layer_histogram(layer_key)`
- `ScopeOne.get_pixel_value(layer_key, x, y)`
- `ScopeOne.get_line_profile(layer_key, x1, y1, x2, y2)`
- `ScopeOne.detect_particles(layer_key, threshold, min_area, max_area, max_particles=1000, export_mask=False, publish_mask=False)`
- `ScopeOne.layer_options()`
- `ScopeOne.set_layer_layout(layout)`
- `ScopeOne.set_visible_layers(layer_keys)`
- `ScopeOne.set_layer_display(layer_key, visible=None, opacity_percent=None, gamma=None, colormap=None, blending=None, levels=None)`
- `ScopeOne.auto_layer_levels(layer_key)`
- `ScopeOne.full_layer_levels(layer_key)`
- `ScopeOne.set_layer_auto_stretch(layer_key, enabled)`
- `ScopeOne.get_source_display_transform(source_id)`
- `ScopeOne.set_source_display_transform(source_id, offset_x=None, offset_y=None, zoom_percent=None, flip_x=None, flip_y=None)`
- `ScopeOne.reset_source_display_transform(source_id)`
- `ScopeOne.move_layer(layer_key, offset)`
- `ScopeOne.config_groups()`
- `ScopeOne.configs(group)`
- `ScopeOne.current_config(group)`
- `ScopeOne.set_config(group, config)`
- `ScopeOne.remove_static_layer(layer_key)`
- `ScopeOne.clear_static_layers()`
- `ScopeOne.create_line_markup(layer_key, x1, y1, x2, y2, label="", role="generic")`
- `ScopeOne.create_rect_markup(layer_key, x, y, width, height, label="", role="generic")`
- `ScopeOne.list_markups(layer_key=None)`
- `ScopeOne.update_markup(markup_id, label=None, visible=None, selected=None, **geometry)`
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
- `ScopeOne.set_half_roi(camera)`
- `ScopeOne.clear_roi(camera="All")`
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
- `ScopeOne.start_stage_mosaic(camera_id, xy_stage_id, rows=1, columns=1, step_x_um=0.0, step_y_um=0.0, settle_ms=150, return_to_start=True, gallery_save_dir=None)`
- `ScopeOne.stage_mosaic_status()`
- `ScopeOne.cancel_stage_mosaic()`
- `ScopeOne.processing_state()`
- `ScopeOne.processing_modules()`
- `ScopeOne.set_processing_bit_depth(bit_depth)`
- `ScopeOne.set_realtime_processing(enabled, camera_id=None)`
- `ScopeOne.start_processing(camera_id=None)`
- `ScopeOne.stop_processing()`
- `ScopeOne.add_processing_module(kind, parameters=None)`
- `ScopeOne.remove_processing_module(index)`
- `ScopeOne.set_processing_module_parameters(index, parameters)`
- `ScopeOne.reset_processing_module_state(index)`
- `ScopeOne.experiment_document()`
- `ScopeOne.validate_experiment(document)`
- `ScopeOne.save_experiment(file_path, document)`
- `ScopeOne.load_experiment(file_path)`
- `ScopeOne.start_experiment(document)`
- `ScopeOne.experiment_status(experiment_id)`
- `ScopeOne.cancel_experiment(experiment_id)`
- `ExperimentSession.experiment_id()`
- `ExperimentSession.status()`
- `ExperimentSession.document()`
- `ExperimentSession.cancel()`
- `ExperimentSession.close()`
- `ScopeOne.frame_mapping_info()`
- `ScopeOne.write_frame_mapping(image, camera="python", bits_per_sample=None, frame_index=0, source_roi=None)`
- `ScopeOne.process_frame_mapping(camera=None, start_module_index=None, end_module_index=None)`
- `ScopeOne.process_image(image, camera="python", bits_per_sample=None, start_module_index=None, end_module_index=None)`
- `ScopeOne.latest_raw_frame(camera)`
- `ScopeOne.layer_frame(layer_key)`
- `ScopeOne.show_frame_mapping_as_layer(layer_id="python_result", name="Python Result", camera=None)`
- `ScopeOne.save_frame_mapping(save_dir, base_name, format="ome-tiff", compression=False, compression_level=6, camera=None)`
- `ScopeOne.continue_pipeline(frame, image=None)`
- `ScopeOne.show_frame(frame, image=None, layer_id="python_result", name="Python Result")`
- `ScopeOne.show_image(image, layer_id="python_result", name="Python Result", camera="python", bits_per_sample=None)`
- `ScopeOne.save_frame(frame, save_dir, base_name, image=None, format="ome-tiff", compression=False, compression_level=6)`
- `ScopeOne.save_image(image, save_dir, base_name, format="ome-tiff", compression=False, compression_level=6, camera="python", bits_per_sample=None)`
- `ScopeOne.record(frames, camera="All", timeout_ms=120000, mda_interval_ms=0.0, z_positions=None, positions=None, order=None)`
- `RecordingSession.camera_ids()`
- `RecordingSession.frame_count(camera=None)`
- `RecordingSession.frame(camera, index)`
- `RecordingSession.process_frame(camera, index, start_module_index=None, end_module_index=None)`
- `RecordingSession.frames(camera)`
- `RecordingSession.save(save_dir, base_name, format="ome-tiff", compression=False, compression_level=6)`
- `RecordingSession.close()`
- `FrameResult.write(image=None)`

`FrameResult` exposes common metadata as typed attributes: `camera`, `width`, `height`, `pixel_format`, `bits_per_sample`, `frame_index`, `timestamp_ns`, `source_roi`, `module_index`, `next_module_index`, and `start_module_index`.

`FrameResult.write(image)` clips and casts the edited pixels to the frame format, writes them into the shared mapping, and updates `frame.image` to the written array.

## Local API Protocol

ScopeOne uses one local control pipe for JSON commands and one shared-memory block for frame transfer.

- Control endpoint on Windows: `\\.\pipe\ScopeOne.Api.local`
- Control endpoint on Linux/Unix: `<tempdir>/ScopeOne.Api.local`
- Message framing: 4-byte little-endian unsigned payload size, followed by UTF-8 JSON.
- Maximum JSON payload: 64 MiB.
- Requests may include a string or numeric `requestId`; every response echoes it.
- Success response: `{"type": "<request type>", "requestId": 1, "ok": true, ...}`
- Error response: `{"type": "<request type>", "requestId": 1, "ok": false, "error": "..."}`

### Control requests

- `ping`: health check.
- `version`: response `version` for ScopeOne and `coreVersion` for ScopeOneCore.
- `status`: response `version`, `coreVersion`, configuration lifecycle state, configuration path, completeness, configuration error, failed configuration devices, cameras, devices, running previews, processing state, layer keys, Stage Mosaic status, recording progress, and writer status. Writer status includes captured, written, and dropped frame counts, written bytes, and queued bytes.
- `capabilities`: response `capabilities` with operation groups and hardware, filesystem, destructive, and long-running operation classifications.
- `state_snapshot`: response `snapshot` with application and Core versions, configuration, hardware inventory, preview, processing, scene, live acquisition and writer progress, experiment, and session state.
- `frame_mapping_info`: response `mappingName`, `mappingSize`, `headerBytes`, `maxPayloadBytes`, and supported `pixelFormats`.
- `load_config`: fields `configPath`; response `state`, `complete`, `cameraIds`, `failedDevices`, `successCount`, `failCount`, and `skippedCameraCount`. A valid configuration can finish as `partially_loaded` when non-camera devices fail; camera backend startup failures are cleaned up and returned as an error.
- `unload_config`: unload current Micro-Manager config and return `state` and `complete`. A failed unload returns `state: "failed"` and an error while preserving the actual hardware inventory for recovery.
- `camera_ids`: response `cameraIds`.
- `loaded_devices`: response `devices`.
- `start_preview`: fields `camera`, accepts a camera id or `"All"`.
- `stop_preview`: fields `camera`, accepts a camera id or `"All"`.
- `image_windows`: response `activeDocumentId` and `documents`; each document contains its ID, title, session, camera, current frame, frame count, readiness, and active state.
- `open_image_window`: fields `sessionId`, optional `title` and `cameraId`; opens matching retained session data, waits for the first frame, and returns `documentIds` and `activeDocumentId`.
- `activate_image_window`: field `documentId`; activates the window and returns `document`.
- `close_image_window`: optional field `documentId`; closes the selected or active window.
- `process_image_window`: optional fields `documentId` and `completeStack`; asynchronously processes the selected or active window and returns the new `document`.
- `save_image_window`: fields `saveDir`, `baseName`, `format` (`ome-tiff`, `ome-zarr`, `tiff` or `binary`), `compression`, and `compressionLevel`, plus optional `documentId`; asynchronously saves the selected or active window and returns `documentId` and `message`.
- `list_layers`: response `layers`; layer display, histogram, pixel, profile, and markup operations target the active image viewer.
- `get_layer_histogram`: fields `layerKey`; response `histogram` with summary statistics and 256 `bins`.
- `get_pixel_value`: fields `layerKey`, `x`, `y`; response `value`.
- `get_line_profile`: fields `layerKey`, `x1`, `y1`, `x2`, `y2`; response `values`.
- `detect_particles`: fields `layerKey`, `threshold`, `minArea`, `maxArea`, optional `maxParticles`, `exportMask`, and `publishMask`; response `particleCount`, effective thresholds, truncation state, particle measurements, optional shared-memory `mask` metadata, and either `maskLayerKey` for Live or `maskDocumentId` for a static image window.
- `layer_options`: response `layouts`, `colormaps`, `blendingModes`.
- `set_layer_layout`: fields `layout`, accepts `side_by_side` or `overlay`.
- `set_visible_layers`: fields `layerKeys`; response `visibleLayers`.
- `set_layer_display`: fields `layerKey`, optional `visible`, `opacityPercent`, `gamma`, `colormap`, `blending`, and display level fields `minLevel`, `maxLevel`, `maxPossible`.
- `auto_layer_levels`: fields `layerKey`; computes and applies histogram-based display levels.
- `full_layer_levels`: fields `layerKey`; restores the full intensity range.
- `set_layer_auto_stretch`: fields `layerKey`, `enabled`; continuously updates display levels from new histograms.
- `get_source_display_transform`: fields `sourceId`; returns source offset, zoom, and flip state.
- `set_source_display_transform`: fields `sourceId` and optional `offsetX`, `offsetY`, `zoomPercent`, `flipX`, `flipY`.
- `reset_source_display_transform`: fields `sourceId`; restores zero offset, 100 percent zoom, and no flips.
- `move_layer`: fields `layerKey`, `offset`; response ordered `layers`.
- `config_groups`: response `groups`.
- `configs`: fields `group`; response `configs`.
- `current_config`: fields `group`; response `config`.
- `set_config`: fields `group`, `config`; response `config`.
- `remove_static_layer`: fields `layerKey`.
- `clear_static_layers`: remove all static preview layers.
- `create_line_markup`: fields `layerKey`, `x1`, `y1`, `x2`, `y2`, optional `label` and `role`; response `markupId`.
- `create_rect_markup`: fields `layerKey`, `x`, `y`, `width`, `height`, optional `label` and `role`; response `markupId`.
- `list_markups`: optional field `layerKey`; response `markups` with `id`, `type`, `role`, `layerKey`, `layerKind`, `sourceId`, `coordinateSpace`, `label`, `visible`, `selected`, and geometry fields.
- `update_markup`: fields `markupId` and optional `label`, `visible`, `selected`, plus line fields `x1`, `y1`, `x2`, `y2` or rect fields `x`, `y`, `width`, `height`; response `markup`.
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
- `set_half_roi`: fields `camera`; response includes read-back ROI geometry.
- `clear_roi`: optional field `camera`, defaults to `All`.
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
- `start_stage_mosaic`: fields `cameraId`, `xyStageId`, and optional `rows`, `columns`, `stepXUm`, `stepYUm`, `settleMs`, `returnToStart`, and `gallerySaveDir`; starts asynchronous mosaic acquisition and returns `status`. `gallerySaveDir` becomes the default directory if the resulting Gallery session is saved later.
- `stage_mosaic_status`: response `status` with `state`, tile progress, message, and completed session ID.
- `cancel_stage_mosaic`: cancels the running mosaic and returns its final `status`.
- `processing_modules`: response `bitDepth`, `realTime`, `realTimeSource`, `modules`, and descriptor list `availableModules`.
- `set_processing_bit_depth`: fields `bitDepth`, accepts `8` or `16`.
- `set_realtime_processing`: fields `enabled` and optional `cameraId`; an empty camera ID selects all cameras.
- `add_processing_module`: fields `kind`, optional `parameters`; response `index`.
- `remove_processing_module`: fields `index`.
- `set_processing_module_parameters`: fields `index`, `parameters`.
- `reset_processing_module_state`: fields `index`.
- `experiment_document`: returns the shared schema version 2 document, initializing a Draft from current cameras, processing, layers, and markups when needed; response `document`.
- `validate_experiment`: field `document`; response contains the canonical validated `document`.
- `save_experiment`: fields `filePath`, `document`; validates and atomically saves the document.
- `load_experiment`: field `filePath`; replaces the shared UI document when no experiment is running and responds with `document`.
- `start_experiment`: field `document`; starts a validated Draft asynchronously and responds with `experimentId`, `state`, and `document`.
- `experiment_status`: field `experimentId`; response `state`, `cancelRequested`, `document`, live `progress` and `writer` state while active, and completed recording session details when available.
- `cancel_experiment`: field `experimentId`; requests cancellation and returns the current experiment status.
- `record`: fields `frames`, `camera`, `timeoutMs`, `mdaIntervalMs`, `zPositions`, `positions`, `order`; response `sessionId`, `cameraIds`.
- `session_info`: fields `sessionId`; response `cameraIds`, `frameCount`, `frameCounts`.
- `session_close`: fields `sessionId`; releases the recorded session held by the app.
- `session_frame`: fields `sessionId`, `camera`, `index`; response `mappingName`, `mappingSize`, and frame metadata.
- `latest_raw_frame`: fields `camera`; response `mappingName`, `mappingSize`, and frame metadata.
- `layer_frame`: field `layerKey`; exports the current raw, processed, static, mosaic, or Gallery layer frame and returns shared-memory metadata.
- `session_process_frame`: fields `sessionId`, `camera`, `index`, optional `startModuleIndex` or `endModuleIndex`; response `mappingName`, `mappingSize`, frame metadata, and optional stage metadata.
- `process_frame_mapping`: optional fields `camera`, `startModuleIndex` or `endModuleIndex`; response `mappingName`, `mappingSize`, frame metadata, and optional stage metadata.
- `show_frame_mapping_as_layer`: optional fields `camera`, `layerId`, `name`; imports the current shared memory frame as a preview layer and returns `layerKey`.
- `save_frame_mapping`: fields `saveDir`, `baseName`, `format` (`ome-tiff`, `ome-zarr`, `tiff` or `binary`), `compression`, `compressionLevel`, optional field `camera`; imports the current shared memory frame and saves it as a one-frame output.
- `session_save`: fields `sessionId`, `saveDir`, `baseName`, `format` (`ome-tiff`, `ome-zarr`, `tiff` or `binary`), `compression`, `compressionLevel`; response `paths`.

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
For timed MDA with more than one time point, `order` must begin with `time` so event start times remain monotonic.

The initially created document is a complete editable Draft with in-memory recording enabled by default. Set `plan.streamToDisk`, `plan.saveDir`, and `plan.baseName` together for streamed output. Experiment documents are parsed strictly: every schema field is required, unknown fields and unsupported schema versions are rejected, and `start_experiment` accepts only Draft documents whose camera IDs are currently available. `start_experiment` is non-blocking; use the returned `ExperimentSession` or the direct status and cancel methods to control the run. Call `ExperimentSession.close()` after completion to release retained recording frames while keeping document status available.

Processing module editing follows the desktop UI rules: stop real-time processing before changing bit depth, adding/removing modules, updating module parameters, or resetting module state. Pass `add_processing_module` a stable module ID returned in `processing_modules.availableModules`; this includes modules supplied by installed processing plugins.

### Frame transfer

`session_frame` writes the selected frame into shared memory:

- Mapping name: `ScopeOne.Api.frame`
- Header layout: `scopeone::core::SharedFrameHeader`
- Pixel data starts at `scopeone::core::kSharedFrameHeaderSize`
- Python reads this through `scopeone.shm.frame_to_ndarray()`
- Responses include `camera`, `width`, `height`, `stride`, string `payloadBytes`, `pixelFormat`, `bitsPerSample`, string `frameIndex`, string `timestampNs`, `sourceRoiX`, `sourceRoiY`, `sourceRoiWidth`, `sourceRoiHeight`, and `sourceRoiValid`.
- Session frames can come from memory, saved OME-TIFF or OME-Zarr outputs, TIFF stacks, or saved binary streams.
- `ScopeOne.latest_raw_frame(...)`, `ScopeOne.layer_frame(...)`, `RecordingSession.frame(...)`, `RecordingSession.frames(...)`, `RecordingSession.process_frame(...)`, and `ScopeOne.process_frame_mapping(...)` return `FrameResult` objects.

`latest_raw_frame` exports the current live raw frame for Python processing, while `layer_frame` accepts any key returned by `list_layers`. Particle detection can return its mask as a `FrameResult` with `export_mask=True` or publish the mask directly with `publish_mask=True`. `session_process_frame` reads a stored session frame, processes it through the current pipeline, and writes the result into the same shared memory block. Use `endModuleIndex` to stop after one stage. The response then includes `moduleIndex` and `nextModuleIndex`. Pass the edited numpy array to `ScopeOne.continue_pipeline(frame, image=edited_image)` to write it back and continue with the next pipeline stage. Pass a frame and optional edited image to `ScopeOne.show_frame(frame, image=edited_image)` to display it in the ScopeOne preview as a static layer. Use `ScopeOne.save_frame(frame, save_dir, base_name, image=edited_image)` to save the current Python/C++ result directly. `FrameResult.write()` requires the shared mapping to still contain the same frame metadata, so request the frame again after another frame export overwrites the mapping. `process_frame_mapping`, `show_frame_mapping_as_layer`, and `save_frame_mapping` reuse the last exported or explicitly imported camera id when `camera` is omitted. Provide `camera` the first time a mapping was written by an external client.

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
