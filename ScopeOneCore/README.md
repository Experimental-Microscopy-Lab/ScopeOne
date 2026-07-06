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
- `startPreview(...)`
- `stopPreview(...)`
- `setExposure(...)`
- `readExposure(...)`
- `setROI(...)`
- `clearROI(...)`
- `getLatestRawFrame(...)`
- `getRawImageStatistics(...)`
- `computeHistogramStats(...)`
- `setLineProfile(...)`
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
- `processFrameAsync(...)`
- `processFrame(...)`
- `processFrameFrom(...)`
- `processFrameThrough(...)`
- `processFrameWithIntermediates(...)`
- `processingModules()`
- `addProcessingModule(...)`
- `removeProcessingModule(...)`
- `setProcessingModuleParameters(...)`
- `resetProcessingModuleState(...)`
- `setRecordingMaxPendingWriteBytes(...)`
- `recordingMaxPendingWriteBytes()`
- `startRecording(...)`
- `stopRecording()`
- `isRecording()`
- `saveRecordingSession(...)`
- `saveRecordingSessionAsync(...)`

## Processing Data Flow

`ImageFrame` is the frame model used by preview, processing, recording, gallery and the local API. Use `processFrameWithIntermediates(...)` to capture each module output. Each `ProcessingStageFrame::nextModuleIndex` can be passed to `processFrameFrom(...)` with an edited frame to continue the same pipeline after that module. Saved TIFF and binary recording outputs are read back through `RecordingSessionData::imageFrameAt(...)` before entering preview or processing again. Live preview processing and synchronous API processing use separate runtime pipeline state so offline frame edits do not change live module buffers.
