# ScopeOneCore

`ScopeOneCore` is the reusable runtime library behind the desktop app.

The desktop app currently assumes `ScopeOneCore` is checked out under the `ScopeOne` repository root.

## Source Layout

```text
ScopeOneCore/
|-- include/scopeone/   Public C++ headers installed for consumers
|-- internal/           Private headers used only while building ScopeOneCore
|-- src/                C++ implementations for both public and private types
|-- python/scopeone/    External Python client for a running ScopeOne app
|-- cmake/              CMake package configuration templates
|-- external/           Third-party dependencies and device adapter sources
|-- build/              Generated build tree
`-- install/            Generated local installation consumed by the desktop app
```

`include/scopeone` defines the installed C++ contract. A header belongs here only when the desktop app or another external consumer must compile against it. Consumers include these files with the installed prefix, for example:

```cpp
#include <scopeone/ScopeOneCore.h>
#include <scopeone/ImageFrame.h>
```

`internal` contains implementation contracts between Core managers, processing modules and DriverHost. These headers are available to the `ScopeOneCore` target through a private include path, are not installed, and may change without preserving source compatibility. Code outside `ScopeOneCore` must not include them.

`src` contains implementations. A public class such as `ScopeOneCore`, `ImageSceneModel` or `ExperimentDocument` still has its `.cpp` file in `src`; being public is determined by its header location and exported API, not by the location of its implementation.

Use this placement rule:

- Put a stable type or function required by consumers in `include/scopeone`.
- Put a Core-only manager, algorithm or protocol detail in `internal`.
- Put executable implementation in `src`.
- Keep desktop widgets and Qt UI behavior in the top-level ScopeOne `src` directory, outside `ScopeOneCore`.

`build` and `install` are generated directories and should not be edited. The desktop app consumes the package from `install` through the exported CMake target instead of reaching into `internal`.

## Namespace Conventions

| Namespace | Purpose | Examples |
|---|---|---|
| `scopeone::core` | Stable Core-facing types and public facades | `ScopeOneCore`, `ImageFrame`, `ExperimentDocument`, `ImageSceneModel` |
| `scopeone::core::internal` | Core-only managers and processing implementations | `CameraManager`, `MMCoreManager`, `RecordingManager`, processing modules |
| `scopeone::core::internal::driverhost` | Shared DriverHost message framing | Versioned request, response and event envelopes |
| `scopeone::ui` | Desktop application widgets and UI coordination outside this library | `MainWindow`, `PreviewWidget`, `InspectWidget` |

Code in `src` that implements a public type remains in `scopeone::core`. Code that implements an `internal` header remains in `scopeone::core::internal`. The Python package named `scopeone` is an external client package and is not an embedded form of the C++ namespace.

The CMake target `scopeone::ScopeOneCore` is a namespaced CMake alias, not a C++ namespace. A consumer normally uses both forms as follows:

```cmake
find_package(ScopeOneCore CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE scopeone::ScopeOneCore)
```

```cpp
scopeone::core::ScopeOneCore core;
```

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
- `build/Release/ScopeOne_DriverHost.exe`
- The external hardware example is built under `plugins/examples/hardware`.
- `build/ScopeOneCoreConfig.cmake`
- `install/bin/ScopeOneCore.dll`
- `install/bin/ScopeOne_DriverHost.exe`
- External hardware plugins are installed under `plugins/hardware`.
- `install/lib/cmake/ScopeOneCore/ScopeOneCoreConfig.cmake`


## Public API

The installed headers are the source of truth for the public API:

- `ScopeOneCore.h` provides the main hardware, acquisition, processing, recording and frame-graph facade.
- `HardwareProvider.h`, `HardwareCapabilities.h` and `CameraProvider.h` define provider discovery, device control and frame delivery.
- `DriverHostProviderPlugin.h` defines the module factory used to load external providers in isolated DriverHost processes.
- `ProcessingPlugin.h` defines processing module descriptors, runtime modules and the external processing plugin contract.
- `ToolPlugin.h` defines the restricted desktop tool context and external tool contract.
- `PluginManifest.h` validates the common plugin identity and API version metadata.
- `HardwareTypes.h` defines provider-independent device identity, state and endpoint metadata.
- `SimulatorProvider.h` provides an in-process reference provider.
- `ImageFrame.h` defines the image payload and metadata exchanged across Core features.
- `ExperimentDocument.h` defines experiment plans, results, persistence and provenance.
- `ImageSceneModel.h` defines shared image-layer, display-state and markup state.
- `SharedFrame.h` defines the language-neutral shared-memory frame layout.
- `scopeone_core_export.h` supplies DLL import and export declarations and is normally included indirectly.

External code should enter through these headers and `scopeone::core::ScopeOneCore`. Internal managers are implementation details and must not become alternate access paths.

Providers use ScopeOne logical device IDs and publish `ImageFrame` objects through `CameraProvider::FrameSink`. Register in-process providers with `ScopeOneCore::registerHardwareProvider(...)`. Submit isolated module loading with `ScopeOneCore::registerDriverHostProvider(providerId, modulePath, options)` and observe `hardwareProviderRegistrationFinished` for the result. One DriverHost process owns the complete Provider and registers all of its cameras and control devices together. Micro-Manager remains the built-in provider, using the native camera path for one camera and isolated DriverHost camera processes for multiple cameras.

## Plugin Boundaries

- `plugins/hardware` contains native `HardwareProvider` modules. Each provider runs in an isolated DriverHost process. Micro-Manager Device Adapters remain under Micro-Manager and are not wrapped as ScopeOne plugins.
- `plugins/processing` contains `ProcessingPlugin` modules loaded by ScopeOneCore. A plugin publishes stable module IDs, parameter descriptors and factories. Built-in processing methods use the same registry.
- `plugins/tools` contains optional desktop `ScopeOneToolPlugin` modules. These receive a restricted UI context rather than direct access to `MainWindow` or `PreviewWidget`. Built-in Scale, Stage Mosaic and Particle Detection tools use the same registry.

External projects consume the exported `scopeone::PluginSDK` CMake target. Every plugin manifest declares `id`, `name`, `version`, `scopeOneApi`, and `kind`; incompatible manifests are rejected before the plugin instance is created.

Hardware and processing contracts are installed public Core APIs. Desktop tool plugins target the ScopeOne application UI contract.

## Processing Data Flow

`ImageFrame` is the frame model used by preview, processing, recording, gallery and the local API. Processing recipes persist stable module IDs rather than registry positions. Available modules and their parameter descriptors come from the processing registry. Use `processFrameThrough(...)` to stop at one pipeline stage and `processFrameFrom(...)` to continue from a later module after an edited frame is written back. Saved OME-TIFF, OME-Zarr, TIFF and binary recording outputs are read back asynchronously through `ScopeOneCore::requestRecordingSessionFrame(...)`.

Real-time processing can consume all camera streams or one camera selected with `setRealTimeProcessingSource(...)`. `requestImageProcessing(...)` applies an isolated pipeline to one current image. `requestRecordingSessionStackProcessing(...)` applies one stateful isolated runtime to a complete session camera stack, reports progress, supports cancellation and creates a new in-memory Gallery session. These offline paths do not change live module buffers. The Local API can list, open, activate, process, save and close independent image windows backed by retained sessions.

Raw live frames, processed live frames, static tool/gallery frames, external API frames and session frame sources are routed through the core frame graph. UI preview widgets keep only a render cache, and callers should use `ScopeOneCore` frame facade methods instead of reading camera managers, recording sessions or preview cache state directly.

`ExperimentPlan` is the single recording and MDA input contract. `ExperimentDocument` adds actual event results, software and device provenance, output files, stable image layers, pixel-to-sensor transforms and markups. Schema version 2 documents include the sample-plane `pixelSizeUm` calibration, are validated strictly, and can be round-tripped with `experimentDocumentToJson(...)`, `saveExperimentDocument(...)` and `loadExperimentDocument(...)`; OME-TIFF is the default disk format. All OME-TIFF, OME-Zarr, plain TIFF and binary frame writing is delegated to the reusable ScopeWriter library. ScopeOne retains acquisition orchestration, experiment documents, output naming and reading.
