# ScopeOneCore

`ScopeOneCore` is the reusable runtime library behind the desktop app and Python bindings.

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
- `python/scopeone/src/scopeone/_core*.pyd`


## Public API


- `ScopeOneCore::getVersion()`
- `hasCore()`
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
- `processingModules()`
- `addProcessingModule(...)`
- `removeProcessingModule(...)`
- `setProcessingModuleParameters(...)`
- `resetProcessingModuleState(...)`
- `setRecordingAvailableCameras(...)`
- `setRecordingMaxPendingWriteBytes(...)`
- `recordingMaxPendingWriteBytes()`
- `startRecording(...)`
- `stopRecording()`
- `isRecording()`
- `saveRecordingSession(...)`
- `saveRecordingSessionAsync(...)`
