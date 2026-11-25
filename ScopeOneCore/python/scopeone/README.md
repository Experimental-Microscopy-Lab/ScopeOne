# ScopeOne Python API

Python client for ScopeOne's language-neutral Local API. It controls a running ScopeOne app and can be used by scripts, notebooks, or Python-based agent tool adapters.

## Installation

Python 3.12 or later.

```powershell
pip install scopeone
```

Start ScopeOne, then check the connection:

```powershell
python -c "from scopeone import ScopeOne; print(ScopeOne().version())"
```

### From source

For changing the client. `requirements.txt` fixes the versions of numpy and pywin32.

```powershell
cd ScopeOneCore\python\scopeone
pip install -r requirements.txt
pip install -e .
```

## Project layout

- Python package project root: `ScopeOneCore/python/scopeone`
- Package source: `ScopeOneCore/python/scopeone/src/scopeone`
- `scopeone.ScopeOne`, `Frame` and `RecordingSession` (in `scopeone.client`) control a running ScopeOne and move frames in and out of it as numpy arrays
- `scopeone.processing` goes the other way: ScopeOne runs Python functions as pipeline modules, see [Python processing modules](#python-processing-modules)
- `scopeone._shm` reads and writes the shared frame layout and is internal

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
the script in `processing/python/` beside ScopeOne or in the same folder under the
user's application data directory. The modules appear in the pipeline after a restart. ScopeOne ships no script
there; `ScopeOneCore/python/scopeone/examples/processing_offset.py` is one to copy.

A module can also report what it measured by returning `(array, measurements)`, where
`measurements` is a list of dicts with `name`, a number `value`, and optional `x` and `y` in image
pixels. Measurements of the live preview go out as the `measurement` event and are what
[triggers](#triggers) act on. Python processing modules never control devices themselves.

```python
@module("my.python.peak", "Peak")
def peak(image):
    y, x = np.unravel_index(np.argmax(image), image.shape)
    return image, [{"name": "peak", "value": float(image[y, x]), "x": int(x), "y": int(y)}]
```

Each module in a pipeline runs in its own Python process (`python -m scopeone.processing
serve script.py`). That interpreter needs numpy and the `scopeone` package
(`pip install ScopeOneCore/python/scopeone`). Choose it under **Settings > Python
Interpreter**, or with `SCOPEONE_PYTHON`, which takes precedence; otherwise ScopeOne runs
`python3` (`python` on Windows). Output printed by a module goes to the log, and an exception
in it shows as the error of that pipeline step.

ScopeOne first runs `python -m scopeone.processing describe script.py`, which prints one JSON
line with the modules of the script and their parameters. While the pipeline runs, each frame is
one JSON line on stdin with `module`, `parameters`, `width`, `height`, `stride`, `format`
(`mono8` or `mono16`), `bits`, and `mapping` and `capacity`, the name and size of the shared
memory that holds the pixels. The reply is one JSON line on stdout with `width`, `height`,
`format`, `bytes` and, when the module measured something, `measurements`, with the result
written to the start of the same memory, or a JSON line with `error`. A result holds at most
twice the bytes of the frame.

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

`capabilities()` returns `operationGroups`, `longRunningOperations`, `hardwareMutationOperations`, `filesystemMutationOperations`, and `destructiveOperations`. `state_snapshot()` returns application and configuration identity, hardware inventory with the `ScopeOneLimit` ranges of the configuration in `hardware.limits` (each `device`, `property`, `min`, `max`), preview state, processing modules, image layers, markups, live acquisition and writer progress, retained experiments, and recording sessions. It intentionally does not poll every hardware property; use `get_property(..., from_cache=False)`, position reads, exposure reads, or ROI reads when an action requires a fresh hardware value.

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
- `ScopeOne.subscribe(events=None)`
- `ScopeOne.unsubscribe(events=None)`
- `ScopeOne.wait_event(names=None, timeout=None)`
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
- `ScopeOne.set_config_preset(group, preset, settings)`
- `ScopeOne.remove_config_preset(group, preset)`
- `ScopeOne.light_sources()`
- `ScopeOne.set_light_source(name, power_device, power_property, enable_device="", enable_property="", on_value="", off_value="", reading_device="", reading_property="")`
- `ScopeOne.remove_static_layer(layer_key)`
- `ScopeOne.clear_static_layers()`
- `ScopeOne.create_line_markup(layer_key, x1, y1, x2, y2, label="", role="generic")`
- `ScopeOne.create_rect_markup(layer_key, x, y, width, height, label="", role="generic")`
- `ScopeOne.list_markups(layer_key=None)`
- `ScopeOne.update_markup(markup_id, label=None, visible=None, selected=None, **geometry)`
- `ScopeOne.remove_markup(markup_id)`
- `ScopeOne.clear_markups(layer_key=None)`

Shapes drawn in the Analyze panel can also be ellipses, polygons and points. `list_markups` returns an `Ellipse` with `x`, `y`, `width` and `height` of its bounding rectangle, a `Polygon` with a `points` list of `x` and `y`, and a `Point` with `x` and `y`. `update_markup` moves and resizes ellipses like rectangles. They cannot be created or have their geometry changed through the API yet.
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
- `ScopeOne.start_processing(camera_id=None)`
- `ScopeOne.stop_processing()`
- `ScopeOne.add_processing_module(kind, parameters=None)`
- `ScopeOne.remove_processing_module(index)`
- `ScopeOne.set_processing_module_parameters(index, parameters)`
- `ScopeOne.reset_processing_module_state(index)`
- `ScopeOne.triggers()`
- `ScopeOne.set_triggers(triggers)`
- `ScopeOne.set_hooks(hooks, timeout_ms=5000)`
- `ScopeOne.hook_reply(hook_id, action="continue", **changes)`
- `ScopeOne.run_hooks(before=None, after=None, experiment_id=None)`
- `ScopeOne.action_log(since=None)`
- `ScopeOne.log(since=None)`
- `ScopeOne.experiment_document()`
- `ScopeOne.validate_experiment(document)`
- `ScopeOne.save_experiment(file_path, document)`
- `ScopeOne.load_experiment(file_path)`
- `ScopeOne.start_experiment(document)`
- `ScopeOne.experiment_status(experiment_id)`
- `ScopeOne.cancel_experiment(experiment_id)`
- `ScopeOne.process_image(image, camera="python", bits_per_sample=None, start_module_index=None, end_module_index=None)`
- `ScopeOne.latest_raw_frame(camera)`
- `ScopeOne.layer_frame(layer_key)`
- `ScopeOne.continue_pipeline(frame, image=None)`
- `ScopeOne.show_frame(frame, image=None, layer_id="python_result", name="Python Result")`
- `ScopeOne.show_image(image, layer_id="python_result", name="Python Result", camera="python", bits_per_sample=None)`
- `ScopeOne.save_frame(frame, save_dir, base_name, image=None, format="ome-tiff", compression=False, compression_level=6)`
- `ScopeOne.save_image(image, save_dir, base_name, format="ome-tiff", compression=False, compression_level=6, camera="python", bits_per_sample=None)`
- `ScopeOne.record(frames, camera="All", timeout_ms=120000, mda_interval_ms=0.0, z_positions=None, positions=None, order=None)`
- `RecordingSession.experiment_id()`
- `RecordingSession.status()`
- `RecordingSession.document()`
- `RecordingSession.cancel()`
- `RecordingSession.camera_ids()`
- `RecordingSession.frame_count(camera=None)`
- `RecordingSession.frame(camera, index)`
- `RecordingSession.process_frame(camera, index, start_module_index=None, end_module_index=None)`
- `RecordingSession.frames(camera)`
- `RecordingSession.save(save_dir, base_name, format="ome-tiff", compression=False, compression_level=6)`
- `RecordingSession.close()`

`record` and `start_experiment` both return a `RecordingSession`, since Core keeps an experiment and the frames it recorded in memory under one id. Wrap the `experimentId` of an `experiment_state` event in `RecordingSession(scope, experiment_id)` to save a recording a trigger started.

`Frame` exposes common metadata as typed attributes: `camera`, `width`, `height`, `pixel_format`, `bits_per_sample`, `frame_index`, `timestamp_ns`, `source_roi`, `module_index`, `next_module_index`, and `start_module_index`.

## Local API Protocol

ScopeOne uses one local control pipe for JSON commands and one shared-memory block for frame transfer.

- Control endpoint on Windows: `\\.\pipe\ScopeOne.Api.local`
- Control endpoint on Linux/Unix: `<tempdir>/ScopeOne.Api.local`
- Message framing: 4-byte little-endian unsigned payload size, followed by UTF-8 JSON.
- Maximum JSON payload: 64 MiB.
- Requests may include a string or numeric `requestId`; every response echoes it.
- Success response: `{"type": "<request type>", "requestId": 1, "ok": true, ...}`
- Error response: `{"type": "<request type>", "requestId": 1, "ok": false, "error": "..."}`
- Event message, only on a subscribed connection: `{"event": "<name>", "time": "2026-11-13T09:30:00.125Z", "data": {...}}`. It never carries a `requestId`, and can arrive between a request and its response.

### Events

`subscribe` with optional field `events` (a list of names; empty or missing means all) starts sending those events on the same connection, and `unsubscribe` with the same field stops them. Both return the subscribed `events`. A subscription ends when the connection closes. `capabilities` lists the names in `events`.

- `config_loaded`: `path`, `success`, `error`.
- `config_unloaded`: `path`, `success`, `error`.
- `preview_state`: `running`, and `cameraIds` of the cameras that are still running.
- `stage_moved`: `device`, `success` and `error`. Read the position with `read_xy_position` or `read_z_position`.
- `experiment_state`: `experimentId` and `state`, `Running` when the experiment starts, then `Completed`, `Canceled` or `Failed` with `error` and the written `paths`.
- `recording_saved`: `sessionId`, `success`, `message`, and `paths`, or `cameraId` when one camera of the session was saved.
- `measurement`: `cameraId`, `moduleId`, `frameIndex` and `measurements` of one frame from a processing module.
- `trigger_fired`: `index` of the trigger, the `measurement` that crossed its threshold (with `cameraId`, `moduleId` and `frameIndex`), `action`, `success` and `error`.
- `log`: one entry of the [log](#log), with `time`, `level`, `source` and `message`.
- `before_event` and `after_event`: hook requests, see [Hooks](#hooks). They go to the connection that set the hooks, without a subscription.

In Python, `subscribe(events=None)` and `unsubscribe(events=None)` change the subscription, and `wait_event(names=None, timeout=None)` returns the next event of those names as a dict, or `None` after `timeout` seconds. Events that arrive while the client waits for a response are kept for `wait_event`.

```python
scope.subscribe(["experiment_state"])
scope.start_experiment(document)
while (event := scope.wait_event(timeout=600)) and event["data"]["state"] == "Running":
    pass
```

### Triggers

A trigger lets Core react to a measurement within the frame that made it, without a client in
the loop. Core checks every measurement of the live processing against the triggers. When one
crosses a threshold, Core performs the action itself, sends `trigger_fired`, and leaves that
trigger alone for `cooldownMs` milliseconds.

- `measurement`: the `name` a processing module reports.
- `cameraId`: the camera it applies to, empty or missing for any camera.
- `compare`: `above` or `below` `threshold`.
- `action`: `record` takes `frames` (default 100) from the camera that measured, as the `record` request does, and keeps the session in memory, and with `preTriggerFrames` (default 0) starts with that many frames the camera took up to the moment the trigger fired, so the session holds `preTriggerFrames + frames` frames in one run of frame indices with their original timestamps. The frame that crossed the threshold is among them, as many frames before the moment of firing as the live processing lags behind the camera, its `frameIndex` in `trigger_fired` and the action log. While such a trigger is set Core keeps the newest frames of its camera in memory and receives every frame of it, and `set_triggers` refuses triggers whose frames would need more memory than recordings may hold for writing. Right after `set_triggers` the buffer holds fewer frames, none at all for a trigger that fires on the first measurement, whose threshold frame then lies before the session, and the recording takes more after the threshold; `apply_preset` sets `preset` of `group` as `set_config` does, within the limits of the configuration; `stop_preview` stops the preview of the camera that measured.
- `cooldownMs`: default 5000.

```python
scope.set_triggers([
    {"measurement": "contrast", "compare": "above", "threshold": 0.02,
     "action": "record", "frames": 500, "cooldownMs": 10000},
])
scope.subscribe(["trigger_fired"])
event = scope.wait_event(timeout=3600)
```

`set_triggers` checks the measurement name, the camera, the action and its preset, and keeps the
old triggers when one is invalid. The triggers live in the running Core and are listed in
`state_snapshot` under `triggers`.

### Hooks

Hooks let a client step into a running experiment at every event, as the acquisition hooks of
pycro-manager do. `set_hooks` with `hooks` (`before_event`, `after_event` or both) and
`timeoutMs` (default 5000) turns them on for the experiments started afterwards; an empty list
turns them off. They belong to the connection that set them and end when it closes; while they
are on, `set_hooks` from another connection is refused.

At `before_event`, before the hardware moves, the event waits for a reply. The client answers
with `hook_reply` and `hookId`, and `action` `continue`, `skip` (the event takes no frames and
leaves no record; in an experiment that runs to its end, OME-TIFF and OME-Zarr files keep the
shape of the plan with empty planes in place of the skipped ones) or `stop` (the experiment ends as `Completed` with the data taken so far).
`continue` may change `x`, `y`, `z` and `exposureMs` of that event; the moves go through the
same limits as any other, and the session records the changed event with `changedByHook`. Only
the connection that set the hooks can answer them. At `after_event` the
frames are taken and in the session, so `latest_raw_frame` or `session_frame` read them; the
answer is `continue` or `stop`. An event without a reply within `timeoutMs` goes on unchanged.

The hook event data holds `hookId`, `experimentId`, `event` (`sequenceIndex`, `timeIndex`,
`positionIndex`, `zIndex`, `channelIndex`, `channelPreset`, `x`, `y`, `z`, `hasXY`, `hasZ`,
`exposureMs`, `cameraIds`) and, after the event, `frames` with `cameraId`, `frameIndex`,
`timestampNs`, `width` and `height`. Hooks apply to experiments that run as events, that is
with positions, Z positions, channels or time intervals. While hooks are on, the Z slices and
channels are taken one by one, not as hardware sequences. Events cannot be added.

`run_hooks(before, after)` answers the hooks until the experiment ends. A function returns
`None` to continue, `"skip"` or `"stop"`, or a dict with `action` and changes. This skips the
positions marked as empty and corrects the focus from the last image of each position:

```python
empty_positions = {3, 7}
focus_shift = {}

def before(event):
    if event["positionIndex"] in empty_positions:
        return "skip"
    if event["hasZ"]:
        return {"z": event["z"] + focus_shift.get(event["positionIndex"], 0.0)}

def after(data):
    frame = scope.latest_raw_frame(data["frames"][0]["cameraId"])
    focus_shift[data["event"]["positionIndex"]] = measure_defocus_um(frame)

scope.set_hooks(["before_event", "after_event"])
session = scope.start_experiment(document)
scope.run_hooks(before, after, session.experiment_id())
scope.set_hooks([])
```

### Action log

Core logs every request listed under `hardwareMutationOperations` or
`filesystemMutationOperations` in `capabilities` when it finishes, and every trigger action. An
entry is

```json
{"time": "2026-11-13T09:41:07.312Z", "origin": "api", "operation": "set_exposure",
 "parameters": {"camera": "Camera", "exposureMs": 20}, "success": true, "error": ""}
```

`origin` is `api` for a Local API request, which covers the MCP server, `console` for a command
typed in the app console, and `trigger` for a trigger action, whose `operation` is the action and
whose `parameters` hold the trigger `index` and the `measurement` that fired it. `parameters` are
the request fields without `type`, `requestId` and `confirm`. Core keeps the last 10000 entries.

When a recording ends, its metadata file (`<baseName>_metadata.json`, the experiment document
written beside the data) gets the entries from the last configuration load before the recording
to its end as `actionLog`. A `record` request is answered when its recording ends, so its own
entry comes after that file; the plan in the same file holds its settings.

### Log

The log holds the messages the console of the app shows: what ScopeOne reports, its warnings and
errors, and what the drivers and the Python processing modules print. An entry is

```json
{"time": "2026-11-23T13:20:04.518Z", "level": "WARNING", "source": "driverhost:example.camera",
 "message": "Exposure out of range"}
```

`level` is `DEBUG`, `INFO`, `WARNING` or `ERROR`. `source` is `scopeone` for the app itself,
`driverhost:<driver>` for a driver process, `driverhost:micro-manager/<camera>` for the process
of a Micro-Manager camera, which each camera has when a configuration has several, and
`python:<module>` for a processing module. `log` returns the last 10000 entries, the `log` event
sends each new one, and the app writes all of them to `ScopeOne.log` in the `logs` folder of its
application data directory, keeping the session before as `ScopeOne.previous.log`.

Micro-Manager and its device adapters keep their own log, as in Micro-Manager: `CoreLog.txt` in
the same folder, and `CoreLog-<camera>.txt` for a camera in its own process. Their errors reach a
client through the response of the request that failed.

Whoever does a piece of work logs it. Core logs what it does itself, such as loading a
configuration, a recording, a trigger or a processing error. The one who sent a request learns how
it ended from the response: a Local API client from its response, the GUI in its status bar and
the log. The commands typed in the console and their responses show only in the console; the
action log records the commands with the origin `console`.

The log is for people reading what happened. A program follows the request responses, the events
and the [action log](#action-log) instead, which say the same in fields.

### Control requests

- `ping`: health check.
- `version`: response `version` for ScopeOne and `coreVersion` for ScopeOneCore.
- `status`: response `version`, `coreVersion`, configuration lifecycle state, configuration path, completeness, configuration error, failed configuration devices, cameras, devices, running previews, processing state, layer keys, Stage Mosaic status, recording progress, and writer status. Writer status includes captured, written, and dropped frame counts, written bytes, and queued bytes.
- `capabilities`: response `capabilities` with operation groups, event names, and hardware, filesystem, destructive, and long-running operation classifications.
- `state_snapshot`: response `snapshot` with application and Core versions, configuration, hardware inventory and limits, preview, processing, triggers, hooks, scene, live acquisition and writer progress, experiment, and session state.
- `frame_mapping_info`: response `mappingName`, `mappingSize`, `headerBytes`, `maxPayloadBytes`, and supported `pixelFormats`.
- `load_config`: fields `configPath`; response `state`, `complete`, `cameraIds`, `failedDevices`, `successCount`, `failCount`, and `skippedCameraCount`. A valid configuration can finish as `partially_loaded` when non-camera devices fail; cameras that fail to start are cleaned up and returned as an error.
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
- `set_config_preset`: fields `group`, `preset`, `settings` as a list of `device`, `property` and `value`; creates or replaces the preset in the loaded configuration.
- `remove_config_preset`: fields `group`, `preset`.
- `light_sources`: response `sources`, each with `name`, `enableDevice`, `enableProperty`, `onValue`, `offValue`, `powerDevice`, `powerProperty`, `readingDevice`, `readingProperty`.
- `set_light_source`: the fields of one source in `light_sources`; `name`, `powerDevice` and `powerProperty` are required, the enable and reading properties are optional and the enable property needs different `onValue` and `offValue`. Adds or replaces the source of that name.
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
- `action_log`: optional field `since`, an ISO 8601 time; response `entries` at or after it, oldest first. See [Action log](#action-log).
- `log`: optional field `since`, an ISO 8601 time; response `entries` at or after it, oldest first. See [Log](#log).
- `triggers`: response `triggers`, see [Triggers](#triggers).
- `set_triggers`: field `triggers`, which replaces all triggers; an empty list removes them. Response `triggers`.
- `set_hooks`: fields `hooks` and optional `timeoutMs`; response `hooks` and `timeoutMs`. See [Hooks](#hooks).
- `hook_reply`: fields `hookId`, `action` (`continue`, `skip` or `stop`) and optional `x`, `y`, `z`, `exposureMs`. Accepted while a configuration operation runs. It fails when no event waits on `hookId`, for example after the timeout.
- `experiment_document`: returns the shared schema version 2 document, initializing a Draft from current cameras, processing, layers, and markups when needed; response `document`.
- `validate_experiment`: field `document`; response contains the canonical validated `document`. A plan whose exposures, positions, Z positions or channel presets leave a limit of the loaded configuration is reported as an error.
- `save_experiment`: fields `filePath`, `document`; validates and atomically saves the document.
- `load_experiment`: field `filePath`; replaces the shared UI document when no experiment is running and responds with `document`.
- `start_experiment`: field `document`; starts a validated Draft asynchronously and responds with `experimentId`, `state`, and `document`.
- `experiment_status`: field `experimentId`; response `state`, `cancelRequested`, `document`, live `progress` and `writer` state while active, and completed recording session details when available.
- `cancel_experiment`: field `experimentId`; requests cancellation and returns the current experiment status.
- `record`: fields `frames`, `camera`, `timeoutMs`, `mdaIntervalMs`, `zPositions`, `positions`, `order`; response `sessionId`, `cameraIds`.
- `session_info`: fields `sessionId`; response `cameraIds`, `frameCount`, `frameCounts`.
- `session_close`: fields `sessionId`; releases the recorded session held by the app together with its experiment document, which then no longer appears among the retained experiments of `state_snapshot`.
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

The initially created document is a complete editable Draft with in-memory recording enabled by default. Set `plan.streamToDisk`, `plan.saveDir`, and `plan.baseName` together for streamed output. Experiment documents are parsed strictly: every schema field is required, unknown fields and unsupported schema versions are rejected, and `start_experiment` accepts only Draft documents whose camera IDs are currently available. `start_experiment` is non-blocking; use the returned `RecordingSession` or the direct status and cancel methods to control the run. Call `RecordingSession.close()` after completion to release retained recording frames while keeping document status available.

#### Multi-channel acquisition

A plan can image several channels, like the multi-dimensional acquisition of Micro-Manager. A channel is a preset of a configuration group, for example the presets of the `Channel` group of a Micro-Manager configuration. The two fields are optional, and a plan without them acquires a single channel:

```json
{
  "order": ["Time", "Z", "Channel"],
  "channelGroup": "Channel",
  "channels": [
    {"preset": "DAPI", "exposureMs": 10.0, "zOffsetUm": 0.0, "zStack": true, "skipFrames": 0},
    {"preset": "FITC", "exposureMs": 20.0, "zOffsetUm": 1.5, "zStack": true, "skipFrames": 0},
    {"preset": "Rhodamine", "exposureMs": 0.0, "zOffsetUm": 0.0, "zStack": false, "skipFrames": 1}
  ]
}
```

- `order` lists the axes from the slowest to the fastest changing one and must contain `Channel` when `channels` is not empty. The channels are imaged in the order of the list.
- Before an event is taken, the preset of its channel is applied, unless the previous event used the same preset.
- `exposureMs` is the exposure of the channel. Zero keeps `plan.exposureMs`.
- `zOffsetUm` moves the focus of the channel. It is added to the Z positions of the plan, or measured from the focus position at the start when the plan has none.
- `zStack: false` images the channel only at the middle Z position, and `skipFrames: n` only at every (n + 1)th time point.
- The channels become the channel axis of the OME-TIFF and OME-Zarr output and are named after their presets. Planes that are not acquired because of `zStack` or `skipFrames` stay empty.

- `color` (`#RRGGBB`, optional) sets the display colour of the channel, which is written to the `Color` of the OME channel.
- A channel may set `cameraId` to be imaged by that camera only. It must be one of the cameras of the plan, and the other cameras leave the planes of the channel empty.
- The channel setup is also stored in the `scopeone.channels` metadata of the OME output (group, preset, exposure, Z offset, Z stack, skip, camera, and the device properties that each preset sets as `Channel<i>.Setting.<device>.<property>`).

#### Positions, timing, shutter and autofocus

These plan fields are optional and follow the Micro-Manager acquisition engine:

- `positions[i].label` names a position (default `Position <i+1>`; labels must be unique) and `positions[i].z` is the focus of that position. The Z positions of the plan are then offsets from the position focus.
- `timeIntervalsMs` lists one interval per gap between time points (`framesPerBurst - 1` values) and replaces `mdaIntervalMs`; the two cannot be combined.
- `keepShutterOpenSlices` / `keepShutterOpenChannels` keep the shutter open between the Z slices or the channels of one block instead of opening it for each image. The shutter state is restored when the run ends.
- `autofocus` runs the autofocus device of the core before the first image of every position at a time point (every `autofocusEveryFrames`-th time point when that is above 1). Z positions that follow are shifted by the distance the autofocus moved the focus.
- `hardwareSequencing` takes the Z slices of one position and channel in a single sequence: the focus device steps through its positions on the triggers of the camera, which runs one sequence acquisition. It is used where the focus device reports a hardware sequence long enough for the slices and the camera is a native single camera, and the slices are taken one by one otherwise. The channels of one Z slice are sequenced the same way when the properties that differ between their presets (a filter wheel, for example) can step through their values on triggers, which needs `Channel` to be the fastest axis.
- `comment` is free text that is stored with the acquisition, as the OME image description and in the `scopeone.acquisition` metadata, which also holds the `AcquisitionOrder` of the axes from the slowest to the fastest.
- Exposure, XY and Z are sent only when they change, and the stage positions, the channel preset and the shutter are restored when the run ends.

Processing module editing follows the desktop UI rules: stop real-time processing before changing bit depth, adding/removing modules, updating module parameters, or resetting module state. Pass `add_processing_module` a stable module ID returned in `processing_modules.availableModules`; this includes the CUDA and ONNX processing modules and Python modules.

### Frame transfer

`session_frame` writes the selected frame into shared memory:

- Mapping name: `ScopeOne.Api.frame`
- Header layout: `scopeone::core::SharedFrameHeader`
- Pixel data starts at `scopeone::core::kSharedFrameHeaderSize`
- Python reads it into `Frame.image`
- Responses include `camera`, `width`, `height`, `stride`, string `payloadBytes`, `pixelFormat`, `bitsPerSample`, string `frameIndex`, string `timestampNs`, `sourceRoiX`, `sourceRoiY`, `sourceRoiWidth`, `sourceRoiHeight`, and `sourceRoiValid`.
- Session frames can come from memory, saved OME-TIFF or OME-Zarr outputs, TIFF stacks, or saved binary streams.
- `ScopeOne.latest_raw_frame(...)`, `ScopeOne.layer_frame(...)`, `RecordingSession.frame(...)`, `RecordingSession.frames(...)`, `RecordingSession.process_frame(...)`, and `ScopeOne.process_image(...)` return `Frame` objects.

`latest_raw_frame` exports the current live raw frame for Python processing, while `layer_frame` accepts any key returned by `list_layers`. Particle detection can return its mask as a `Frame` with `export_mask=True` or publish the mask directly with `publish_mask=True`. `session_process_frame` reads a stored session frame, processes it through the current pipeline, and writes the result into the same shared memory block. Use `endModuleIndex` to stop after one stage. The response then includes `moduleIndex` and `nextModuleIndex`. Pass the edited numpy array to `ScopeOne.continue_pipeline(frame, image=edited_image)` to write it back and continue with the next pipeline stage. Pass a frame and optional edited image to `ScopeOne.show_frame(frame, image=edited_image)` to display it in the ScopeOne preview as a static layer. Use `ScopeOne.save_frame(frame, save_dir, base_name, image=edited_image)` to save the current Python/C++ result directly.

Python can also send a new numpy image without first requesting a ScopeOne frame: `ScopeOne.process_image(...)` runs it through the pipeline, `ScopeOne.show_image(...)` shows it as a layer and `ScopeOne.save_image(...)` saves it. The shared frame mapping these methods use is internal to the client; other clients use it through the Local API requests listed above.

### Error policy

The server returns `ok: false` and a short actionable `error` string. Recording errors distinguish startup failure, timeout before session data, MDA captured no frames, preview-frame recording captured no frames, and invalid session/frame indexes.

## Notes

- `ScopeOne()` connects to a running ScopeOne control server over the local platform endpoint.
- Start the ScopeOne app before using the Python API.
- Python 3.12 or later is required.
- Frame reads use a shared-memory export block.
- The package itself depends on `numpy` and `pywin32`. `Pillow` is only needed if you want notebook/image display helpers in your own code.
- Runnable examples are in `examples/`: `example_minimal.ipynb` for a first session, `demo_mmconfig_api_walkthrough.ipynb` for most of the API on the demo configuration, `example_config_stage_record.py` for configuration, stage and recording, `example_hooks.py` for acquisition hooks, `example_image_scene_sync.py` for markups, `processing_offset.py` for a processing module, and for the iSCAT Landing scene of the example camera `processing_landing.py`, a module that measures each particle landing, and `example_landing.py`, which builds the pipeline and the trigger around it and saves every recording.

## Build

Build the desktop app when needed:

```powershell
.\scripts\build.ps1 --target all
```
