# ScopeOneCore

`ScopeOneCore` is the reusable runtime library behind the desktop app.

The desktop app currently assumes `ScopeOneCore` is checked out under the `ScopeOne` repository root.

## Build

Run from the `ScopeOneCore` repository root.

Configure:

```powershell
cmake -S . -B build
```

Build:

```powershell
cmake --build build --config Release --parallel
```

Install:

```powershell
cmake --install build --config Release
```


Outputs:

- `build/Release/ScopeOneCore.dll`
- `build/Release/ScopeOneCore.lib`
- `build/Release/ScopeOne_Agent.exe`
- `build/ScopeOneCoreConfig.cmake`
- `install/bin/ScopeOneCore.dll`
- `install/bin/ScopeOne_Agent.exe`
- `install/lib/cmake/ScopeOneCore/ScopeOneCoreConfig.cmake`


## Public API


- `ScopeOneCore::getVersion()`
- `loadConfiguration(...)`
- `unloadConfiguration()`
- `cameraIds()`
- `rawLayerKey(...)`
- `processedLayerKey(...)`
- `staticLayerKey(...)`
- `sourceIdFromLayerKey(...)`
- `isRawLayerKey(...)`
- `isProcessedLayerKey(...)`
- `isStaticLayerKey(...)`
- `startPreview(...)`
- `stopPreview(...)`
- `setExposure(...)`
- `readExposure(...)`
- `setROI(...)`
- `clearROI(...)`
- `graphFrame(...)`
- `graphFrames(...)`
- `graphPixelValue(...)`
- `sessionFrameAt(...)`
- `firstSessionFrames(...)`
- `createFrameSession(...)`
- `publishStaticFrame(...)`
- `publishExternalFrame(...)`
- `removeStaticFrame(...)`
- `clearStaticFrames(...)`
- `clearLiveFrames(...)`
- `clearProcessedFrames(...)`
- `getRawImageStatistics(...)`
- `computeHistogramStats(...)`
- `setLineProfile(...)`
- `setStaticLineProfile(...)`
- `clearLineProfile()`
- `xyStageDevices()`
- `zStageDevices()`
- `currentXYStageDevice()`
- `currentFocusDevice()`
- `readXYPosition(...)`
- `readZPosition(...)`
- `moveXYRelative(...)`
- `moveZRelative(...)`
- `loadedDevices()`
- `deviceProperties(...)`
- `devicePropertyNames(...)`
- `getPropertyValue(...)`
- `propertyTypeString(...)`
- `isPropertyReadOnly(...)`
- `getAllowedPropertyValues(...)`
- `getPropertyLimits(...)`
- `setPropertyValue(...)`
- `isRealTimeProcessingEnabled()`
- `setRealTimeProcessingEnabled(...)`
- `processFrame(...)`
- `processFrameFrom(...)`
- `processFrameThrough(...)`
- `processingModules()`
- `processingRecipe()`
- `applyProcessingRecipe(...)`
- `addProcessingModule(...)`
- `removeProcessingModule(...)`
- `setProcessingModuleParameters(...)`
- `resetProcessingModuleState(...)`
- `setRecordingMaxPendingWriteBytes(...)`
- `recordingMaxPendingWriteBytes()`
- `startRecording(...)`
- `stopRecording()`
- `isRecording()`
- `setRecordingSessionPresentation(...)`
- `saveRecordingSession(...)`
- `saveRecordingSessionAsync(...)`

## Processing Data Flow

`ImageFrame` is the frame model used by preview, processing, recording, gallery and the local API. Use `processFrameThrough(...)` to stop at one pipeline stage and `processFrameFrom(...)` to continue from a later module after an edited frame is written back. Saved TIFF and binary recording outputs are read back through `ScopeOneCore::sessionFrameAt(...)` or `ScopeOneCore::firstSessionFrames(...)` before entering preview or processing again. Live preview processing and synchronous API processing use separate runtime pipeline state so offline frame edits do not change live module buffers.

Raw live frames, processed live frames, static tool/gallery frames, external API frames and session frame sources are routed through the core frame graph. UI preview widgets keep only a render cache, and callers should use `ScopeOneCore` frame facade methods instead of reading camera managers, recording sessions or preview cache state directly.

`ExperimentPlan` is the single recording and MDA input contract. `ExperimentDocument` adds actual event results, software and device provenance, output files, stable image layers, pixel-to-sensor transforms and markups. Schema version 1 documents are validated strictly and can be round-tripped with `experimentDocumentToJson(...)`, `saveExperimentDocument(...)` and `loadExperimentDocument(...)`; image payloads remain in TIFF, binary files or shared memory.
