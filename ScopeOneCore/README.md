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

`internal` contains implementation contracts between Core managers, processing modules and the camera agent. These headers are available to the `ScopeOneCore` target through a private include path, are not installed, and may change without preserving source compatibility. Code outside `ScopeOneCore` must not include them.

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
| `scopeone::core::internal` | Core-only managers and processing implementations | `MMCoreManager`, `RecordingManager`, processing modules |
| `scopeone::core::internal::agent` | Private camera-agent protocol details | Agent request, response and frame transport types |
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
- `build/Release/ScopeOne_Agent.exe`
- `build/ScopeOneCoreConfig.cmake`
- `install/bin/ScopeOneCore.dll`
- `install/bin/ScopeOne_Agent.exe`
- `install/lib/cmake/ScopeOneCore/ScopeOneCoreConfig.cmake`


## Public API

The installed headers are the source of truth for the public API:

- `ScopeOneCore.h` provides the main hardware, acquisition, processing, recording and frame-graph facade.
- `ImageFrame.h` defines the image payload and metadata exchanged across Core features.
- `ExperimentDocument.h` defines experiment plans, results, persistence and provenance.
- `ImageSceneModel.h` defines shared image-layer, display-state and markup state.
- `SharedFrame.h` defines the language-neutral shared-memory frame layout.
- `scopeone_core_export.h` supplies DLL import and export declarations and is normally included indirectly.

External code should enter through these headers and `scopeone::core::ScopeOneCore`. Internal managers are implementation details and must not become alternate access paths.

## Processing Data Flow

`ImageFrame` is the frame model used by preview, processing, recording, gallery and the local API. Use `processFrameThrough(...)` to stop at one pipeline stage and `processFrameFrom(...)` to continue from a later module after an edited frame is written back. Saved TIFF and binary recording outputs are read back through `ScopeOneCore::sessionFrameAt(...)` or `ScopeOneCore::firstSessionFrames(...)` before entering preview or processing again. Live preview processing and synchronous API processing use separate runtime pipeline state so offline frame edits do not change live module buffers.

Raw live frames, processed live frames, static tool/gallery frames, external API frames and session frame sources are routed through the core frame graph. UI preview widgets keep only a render cache, and callers should use `ScopeOneCore` frame facade methods instead of reading camera managers, recording sessions or preview cache state directly.

`ExperimentPlan` is the single recording and MDA input contract. `ExperimentDocument` adds actual event results, software and device provenance, output files, stable image layers, pixel-to-sensor transforms and markups. Schema version 1 documents are validated strictly and can be round-tripped with `experimentDocumentToJson(...)`, `saveExperimentDocument(...)` and `loadExperimentDocument(...)`; image payloads remain in TIFF, binary files or shared memory.
