# ScopeOne tool routing

Use `capabilities` and the live MCP schemas as the complete API reference. This
file records workflow choices that are easy to get wrong; it does not duplicate
the full tool catalog.

## Contents

- [Start every runtime task](#start-every-runtime-task)
- [Route compound requests efficiently](#route-compound-requests-efficiently)
- [Configuration and preview](#configuration-and-preview)
- [Hardware control](#hardware-control)
- [Layers, markups, and analysis](#layers-markups-and-analysis)
- [Processing and frame transfer](#processing-and-frame-transfer)
- [Acquisition and persistence](#acquisition-and-persistence)
- [Failure routing](#failure-routing)

## Start every runtime task

1. Call `capabilities` once per session to discover operation groups and safety
   classifications. Refresh it after an application or server version change.
2. Call `state_snapshot` and identify the exact configuration, camera, device,
   layer, markup, experiment, and session IDs needed for the request.
3. Serialize calls through one control connection. Do not issue concurrent
   mutations or concurrent frame-exporting operations.
4. Before a confirmed operation, state the concrete hardware, filesystem, or
   destructive effect and obtain the user's approval.
5. Read back the affected hardware or application state after every mutation.

Use `ping` only for connection health, `version` for component identity, and
`status` for a compact summary. Use `state_snapshot` for operational decisions.

## Route compound requests efficiently

- Treat a clear user command as approval for the exact confirmed operations it
  names. Ask again only when the target, destination, destructive scope, or
  physical effect is ambiguous or would be inferred.
- Discover capabilities once and resolve identifiers once for a planned
  sequence. Refresh only after an operation invalidates identifiers or a call
  reports stale state.
- Use an operation's returned applied value as its readback when available. Add
  a targeted getter only when the response omits the resulting hardware or
  application state.
- Execute dependent operations in order and stop the sequence at the first
  failure. Do not continue with assumptions about an earlier result.
- For “load this cfg, use half ROI, preview, then auto levels,” use
  `load_config`, resolve the camera, `set_half_roi`, `start_preview`,
  `list_layers`, and `auto_layer_levels`. Do not call `get_layer_histogram` and
  do not repeat `capabilities` or a full `state_snapshot` between those steps.

## Configuration and preview

### Load, switch, or unload configuration

1. Resolve an absolute `.cfg` path before calling `load_config`. Do not open the
   desktop file dialog.
2. After loading, use the returned camera IDs. Call `loaded_devices` only when
   the next action targets a device, and call `state_snapshot` only when broader
   application state is needed.
3. Before changing a preset, call `config_groups`, `configs`, and
   `current_config`. Call `set_config` only with a listed group and preset, then
   read `current_config` again.
4. Treat `unload_config` as destructive because it stops acquisition and
   invalidates device, camera, preview, and layer identifiers. Confirm it and
   verify the unloaded state before continuing.

### Start or stop preview

1. Select one concrete camera unless the user explicitly requests `All`.
2. Call `start_preview` or `stop_preview` after confirmation.
3. Use `state_snapshot.preview.runningCameraIds` when running-camera state is
   needed, or `list_layers` when the next action targets an image layer. Do not
   call both by default.
4. Do not infer that changing one camera should change every camera.

## Hardware control

### Change a property or exposure

1. Resolve the device with `loaded_devices`; use `device_property_names` or
   `device_properties` to inspect valid properties and metadata.
2. Read the current value from hardware with `get_property` using
   `fromCache=false`, or use `read_exposure`, before deciding on a change.
3. Submit property values in the form accepted by Micro-Manager. Do not append
   display units or write a read-only property.
4. After confirmation, call `set_property` or `set_exposure` and perform a fresh
   hardware read. Report the applied value rather than only the requested one.
5. When targeting `All`, preserve and report per-camera results.

### Distinguish camera ROI from an ROI markup

- `set_roi`, `set_half_roi`, and `clear_roi` change camera acquisition hardware.
  They require confirmation and affect future frames.
- `create_rect_markup(role="roi")` creates a visual image-space annotation. It
  does not crop the camera or change acquired pixels.
- For a hardware ROI, identify one camera, read `get_roi`, apply the requested
  operation, and report the returned rectangle when `readBack=true`. Call
  `get_roi` again only when the operation did not return a hardware readback;
  `clear_roi` always needs a separate read when the final rectangle matters.
- Never translate an annotation request into a hardware ROI change, or the
  reverse, without explicit user intent.

### Move an XY or Z stage

1. Resolve the target with `xy_stage_devices`, `z_stage_devices`,
   `current_xy_stage_device`, or `current_focus_device`. Ask when multiple
   devices make the target ambiguous.
2. Read the current position immediately before movement.
3. Confirm the selected device, absolute target or relative offset, and device
   units. Do not assume a unit that the device has not established.
4. Call exactly one of `move_xy_relative`, `move_z_relative`, `move_xy_to`, or
   `move_z_to`, then verify with `read_xy_position` or `read_z_position` and
   report the actual result.
5. Do not blindly retry a failed or partially completed physical movement.

## Layers, markups, and analysis

### Manage image layers and source alignment

1. Call `list_layers` before using a `layerKey` or `sourceId`; use
   `layer_options` before selecting layouts, colormaps, or blending modes.
2. Use `set_layer_layout`, `set_visible_layers`, and `set_layer_display` only for
   presentation. Preserve unrequested display fields.
3. Use source transforms for visual alignment only. Read
   `get_source_display_transform`, apply the requested values with
   `set_source_display_transform` or `reset_source_display_transform`, then read
   it again. A `sourceId` is not interchangeable with a `layerKey`.
4. After `move_layer`, refresh `list_layers` because order is positional.
5. Confirm `remove_static_layer` or `clear_static_layers`. Never pass a live
   camera layer to static-layer removal, and refresh layers and markups after it.

### Adjust display levels

1. Select the intended layer with `list_layers`.
2. Call `auto_layer_levels` directly for one-shot histogram-based levels. Do not
   call `get_layer_histogram` first unless statistics were requested.
3. Use `set_layer_auto_stretch` only when levels should follow new frames.
4. Use `full_layer_levels` to restore the complete intensity range.

### Manage markups

1. Bind every markup to an existing `layerKey` and use image-space coordinates.
2. Use `create_line_markup` with role `cross_section` for a persistent visible
   profile or role `measurement` for a measurement line. Use
   `create_rect_markup` with role `roi` only for an annotation ROI.
3. Capture the returned markup ID. Call `list_markups` before `update_markup` or
   deletion, then verify label, visibility, selection, and geometry afterward.
4. Supply geometry fields appropriate to the existing line or rectangle type.
5. Treat `remove_markup` and `clear_markups` as destructive. When clearing one
   layer, always provide its `layerKey`; omit it only when the user explicitly
   requests removal of all markups.

### Inspect or analyze an image

- Use `get_pixel_value` for one image-space coordinate.
- Use `get_line_profile` for a one-shot profile; use a cross-section markup when
  the profile should remain visible and follow scene updates.
- Use `get_layer_histogram` only when histogram data or summary statistics are
  required.
- For `detect_particles`, validate the threshold and pixel-area limits against
  the selected layer. Use `publishMask=true` only when the mask should appear as
  a static layer. Use `exportMask=true` only when mask pixels are needed, and
  then follow the shared-frame rules below.

## Processing and frame transfer

### Configure the processing pipeline

1. Read `processing_modules` and remember whether real-time processing is
   running.
2. Stop real-time processing before changing bit depth, adding or removing
   modules, changing parameters, or resetting accumulated module state.
3. Use `set_processing_bit_depth`, `add_processing_module`, or
   `set_processing_module_parameters` only from current module data and the live
   schema; preserve fields the user did not request changing.
4. Treat module indexes as positional. Refresh `processing_modules` after every
   add or remove before issuing another index-based operation.
5. Confirm `remove_processing_module`. Call `reset_processing_module_state`
   only while that module still exists and only when its accumulated state
   should be discarded.
6. Restore real-time processing with `set_realtime_processing` only if it was
   previously running or the user requests it, then verify processing state and
   processed layers.

### Choose the correct frame source

- Use `latest_raw_frame` for the newest raw frame from one camera.
- Use `layer_frame` for the current frame of a raw, processed, static, mosaic,
  or Gallery layer.
- Use `session_frame` for one retained recording frame.
- Use `session_process_frame` when a retained frame must run through a selected
  range of the current processing pipeline.
- Use `frame_mapping_info` only when mapping limits or supported pixel formats
  are needed; it does not export a frame.
- Prefer semantic analysis tools over exporting pixels when the requested
  result is already available through histogram, pixel, profile, or particle
  operations.

### Complete a shared-frame workflow

The shared-memory mapping contains one frame at a time for all clients.
`latest_raw_frame`, `layer_frame`, `session_frame`, `session_process_frame`,
`process_frame_mapping`, and `detect_particles(exportMask=true)` can replace its
contents.

1. Export exactly one intended source into the mapping.
2. Immediately read or copy it, or call `process_frame_mapping` with validated
   module bounds.
3. Immediately consume the result with `show_frame_mapping_as_layer` or
   `save_frame_mapping`, or copy its pixels in a client that supports reading.
4. Refresh `list_layers` after publishing a layer. Confirm the destination and
   base name before saving, and report the resulting path.
5. If any other frame-exporting operation may have run, request the original
   frame again instead of trusting the mapping metadata.

## Acquisition and persistence

### Run an experiment or recording

1. Use `experiment_document` as the canonical starting document. Modify only
   requested fields and preserve its schema.
2. Call `validate_experiment` and use the returned normalized document before
   `start_experiment` or `save_experiment`.
3. Treat `load_experiment` as destructive because it replaces the current
   document. Confirm it, then read `experiment_document` again.
4. Confirm output directories and base names before acquisition or saving. Do
   not reuse a base name when overwrite was not explicitly requested.
5. Poll `experiment_status` by its returned experiment ID until a terminal
   state. Report acquisition progress, writer state, errors, and output paths.
6. Call `cancel_experiment` only after an explicit request, then poll until
   cancellation is terminal.
7. Use `record` for a short blocking retained capture. `record(frames=1)` is the
   semantic snapshot operation; do not simulate a Gallery button. The same
   control connection cannot service polling while `record` is blocked.
8. Use `session_info`, `session_frame`, `session_process_frame`, and
   `session_save` before `session_close`. Confirm closing because it discards
   retained session state.

### Run a Stage Mosaic

1. Verify the camera and XY stage IDs and read the current XY position.
2. Confirm rows, columns, pixel size, step sizes, settling time, optional save
   directory, and whether to return to the initial position.
3. Call `start_stage_mosaic`, then poll `stage_mosaic_status` until completed,
   canceled, or failed.
4. Call `cancel_stage_mosaic` only after explicit approval. Do not start another
   mosaic while one is active.
5. Use the returned session ID for frame access and saving. Read the final stage
   position when return-to-start behavior matters.

## Failure routing

- No Claude MCP tools: verify the plugin's configured MCP executable and reload
  plugins. No Codex MCP tools: inspect its `scopeone` registration and restart.
- MCP process cannot start: correct its executable path; do not launch it as a
  separate background process.
- Local API unavailable: start ScopeOne and retry after the desktop app is ready.
- Unknown or stale camera, device, layer, markup, module, experiment, or session:
  refresh `state_snapshot` and the relevant list operation; do not guess IDs.
- Hardware mutation failed: read actual hardware state and report the exact
  error; do not loop retries.
- Long-running call timed out: inspect status before deciding whether another
  start would duplicate active work.
- Shared frame content is stale or ambiguous: export the intended source again.
- File save failed: report the destination and error; do not select a different
  path or overwrite a file without approval.
- Confirmation rejected or absent: stop without attempting an alternate route.
