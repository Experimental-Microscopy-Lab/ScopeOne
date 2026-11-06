# ScopeOneCore

`ScopeOneCore` is the reusable runtime library behind the desktop app.

The desktop app consumes `ScopeOneCore` through its installed CMake package.

## Source Layout

```text
ScopeOneCore/
|-- include/scopeone/   Core facade and public models used by the app
|-- sdk/                Driver SDK: its headers and the small library behind them
|-- internal/           Private headers used only while building ScopeOneCore
|-- src/                C++ implementations, with the CUDA and ONNX modules in src/cuda and src/inference
|-- python/scopeone/    External Python client for a running ScopeOne app
|-- cmake/              CMake package configuration templates
|-- external/           Third-party dependencies and device adapter sources
|-- build/              Generated build tree
`-- install/            Generated local installation consumed by the desktop app
```

`include/scopeone` contains the Core facade, Core-owned public models and the processing module types:

```cpp
#include <scopeone/ScopeOneCore.h>
#include <scopeone/ExperimentDocument.h>
```

`sdk/include/scopeone` contains the driver contract and the types it carries, such as `Driver`, `HardwareProvider`, `CameraProvider`, `SignalSourceProvider`, `DaqProvider` and `ImageFrame`. These headers keep the `scopeone/` include prefix. They build the `ScopeOneDriverSDK` library (`scopeone::DriverSDK`), which Core links publicly. A driver links only that target, so it sees neither the other Core headers nor the Core library, and on Linux it is linked with `--no-undefined`. An SDK header includes only Qt and other SDK headers.

`internal` contains implementation contracts between Core managers, processing modules and DriverHost. These headers are available to the `ScopeOneCore` target through a private include path, are not installed, and may change without preserving source compatibility. Code outside `ScopeOneCore` must not include them.

`src` contains implementations. A public class such as `ScopeOneCore`, `ImageSceneModel` or `ExperimentDocument` has its `.cpp` file in `src` too.

Use this placement rule:

- Put what drivers are written against in `ScopeOneCore/sdk`, and keep it small.
- Put every other public Core model and contract in `ScopeOneCore/include/scopeone`.
- Put a Core-only manager, algorithm or protocol detail in `internal`.
- Put executable implementation in `src`.
- Keep desktop widgets and Qt UI behavior in the top-level ScopeOne `src` directory, outside `ScopeOneCore`.

`build` and `install` are generated directories and should not be edited. The desktop app consumes the package from `install` through the exported CMake target instead of reaching into `internal`.

## Namespace Conventions

| Namespace | Purpose | Examples |
|---|---|---|
| `scopeone::core` | Stable Core-facing types and public facades | `ScopeOneCore`, `ExperimentDocument`, `ImageSceneModel` |
| `scopeone::core` driver contract | Stable types of `DriverSDK.h` | `Driver`, `HardwareProvider`, `CameraProvider`, `SignalSourceProvider`, `DaqProvider`, `ImageFrame` |
| `scopeone::core::internal` | Core-only managers, processing modules and built-in devices | `HardwareRuntime`, `MicroManagerProvider`, `RecordingManager`, `IscatModule`, `SignalSourceManager` |
| `scopeone::core::internal::driverhost` | Shared DriverHost message framing | Versioned request, response and event envelopes |
| `scopeone::ui` | Desktop application widgets and UI coordination outside this library | `MainWindow`, `PreviewWidget`, `DhmTool`, `ToolRegistry` |
| `scopeone::core::internal::cuda`, `scopeone::core::internal::inference` | The CUDA and ONNX processing modules in `src/cuda` and `src/inference` | `core::internal::cuda` |

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
- Drivers are built by the `drivers` project.
- `build/ScopeOneCoreConfig.cmake`
- `install/bin/ScopeOneCore.dll`
- `install/bin/ScopeOne_DriverHost.exe`
- Drivers are installed under `drivers`.
- `install/lib/cmake/ScopeOneCore/ScopeOneCoreConfig.cmake`


## Core and SDK API

The installed headers in `include/scopeone` and `include/sdk/scopeone` are the source of truth for the public API:

- `ScopeOneCore.h` provides the main hardware, acquisition, processing, recording and frame-graph facade.
- `ProcessingPipeline.h`, `ExperimentDocument.h` and `ImageSceneModel.h` are Core-owned runtime models and services.
- `DriverSDK.h` includes the headers of the public driver contract: `CameraProvider.h`, `DaqDevice.h`, `DaqProvider.h`, `Driver.h`, `HardwareCapabilities.h`, `HardwareProvider.h`, `HardwareTypes.h`, `ImageFrame.h`, `SignalSource.h` and `SignalSourceProvider.h`. The `scopeone::DriverSDK` CMake target provides them.
- `ProcessingTypes.h` holds the processing module base class and the descriptors of modules and their parameters.
- `DriverManifest.h` is how ScopeOne discovers and enables drivers. `SharedFrame.h` and `DhmReconstruction.h` serve the application. None of them are driver contracts.
- `scopeone_core_export.h` supplies DLL import and export declarations and is normally included indirectly.

External code should enter through these headers and `scopeone::core::ScopeOneCore`. Internal managers are implementation details and must not become alternate access paths.

Providers use ScopeOne logical device IDs and publish `ImageFrame` objects through `CameraProvider::FrameSink`. The sink copies a frame before it returns, so a frame can point into the buffer of the camera SDK through `QByteArray::fromRawData` instead of being copied by the driver first. Core loads a driver with `ScopeOneCore::registerDriverHostProvider(providerId, modulePath, options)`, which returns false with an error message when the DriverHost cannot start it. One DriverHost process owns the complete Provider and registers all of its cameras and control devices together. Micro-Manager remains the built-in provider. It runs the camera of a configuration with one camera in ScopeOne, and each camera of a configuration with more in a DriverHost of its own, so two cameras of the same model do not share one vendor library.

Inside Core, the rest of Core reaches hardware only through `HardwareRuntime`. It routes each call to the provider of the device and answers the calls that span providers itself, such as all cameras, the default stages and shutter, and the frame delivery to the live view, live processing and recording. Every provider, `MicroManagerProvider` as well as each driver, offers its devices through the same interfaces of the driver SDK, so a driver can do what Micro-Manager does here, from hardware sequences to autofocus. How
frames are delivered and paused is ScopeOne's own policy and stays out of the driver SDK.

## Driver and Processing Boundaries

- `drivers` contains the drivers, among them NI-DAQmx and the example camera and PMT. Each provider runs in an isolated DriverHost process, so a vendor library such as NI-DAQmx is never loaded into ScopeOne. Micro-Manager Device Adapters remain under Micro-Manager and are not wrapped as ScopeOne drivers.
- Python processing modules are scripts in `processing/python`, beside the executable or in the application data directory. Core runs each module of a pipeline in its own Python process.
- The CUDA and ONNX processing modules are part of Core. CUDA is built when nvcc is found and `SCOPEONE_ENABLE_CUDA` is on. ONNX inference is always built, with the official ONNX Runtime package that the configure downloads into `external`, the GPU package when CUDA is built and the CPU package otherwise. `ONNXRUNTIME_ROOT` points Core at another SDK. Every processing method and the DHM reconstruction are part of Core.
- The tool windows (Scale, Stage Mosaic, Particle Detection, DHM Reconstruction, Scanning and DAQ Control, Signal Monitor) are part of the application. They use the `ScopeOneCore` facade, and `ScopeOneToolContext` in `src/tools/ToolContext.h` gives them the desktop operations such as showing layers.

Drivers outside this repository consume the exported `scopeone::DriverSDK` CMake target. Every driver manifest declares `id`, `name`, `version`, `scopeOneApi`, and `kind`, and incompatible manifests are rejected before the library instance is created.

## Processing Modules

Every processing method of Core is a `ProcessingModule` that registers itself at the end of its own source file:

```cpp
SCOPEONE_REGISTER_PROCESSING_MODULE(
    GaussianBlurModule,
    QStringLiteral("gaussian_blur"),
    QStringLiteral("Gaussian Blur"),
    1,
    {integerParameter("kernel_size", "Kernel size", 3, 1, 99, 2),
     realParameter("sigma", "Sigma", 0.0, 0.0, 100.0, 0.1, 2)})
```

The arguments after the class are its `ProcessingModuleDescriptor`: the id that recipes store, the name shown in the processing list, the schema version and the parameters, built with `integerParameter`, `realParameter`, `booleanParameter` and `choiceParameter` from `internal/ProcessingParameters.h`. A file that builds several modules from one table, or a descriptor that depends on the machine such as the ONNX providers, passes a function to `addProcessingModuleRegistration` instead, as `ImageOperationModules.cpp` does. Each file registers once. CMake picks up every source file of Core from its folder, so a new module is a new file and nothing else changes. The processing list is sorted by name.

## CUDA Modules

A CUDA module is the GPU version of a processing method. It is built into Core when CUDA is built, works on one frame at a time like any other module, and is named after the CPU method with "(CUDA)", so the two stand together in the list. Its id starts with `cuda.` and it uses the parameter keys of the CPU method.

`CudaModule` does the frame handling. It uploads the frame into a float image in GPU memory, calls `processDevice`, and converts the result back to an 8 or 16 bit frame. A module writes `processDevice` and its kernels:

```cpp
// internal/cuda/CudaInvertModule.h
#pragma once

#include "internal/cuda/CudaModule.h"

namespace scopeone::core::internal::cuda
{
    class CudaInvertModule : public CudaModule
    {
    public:
        QString id() const override { return QStringLiteral("cuda.invert"); }
        QString name() const override { return QStringLiteral("Invert (CUDA)"); }
        QVariantMap parameters() const override { return {}; }
        void setParameters(const QVariantMap&) override {}
        std::unique_ptr<ProcessingModule> createRuntime() const override
        {
            return std::make_unique<CudaInvertModule>();
        }

    protected:
        bool processDevice(const GpuImage& input, GpuImage& output, int bitDepth) override;
    };
}
```

```cpp
// src/cuda/CudaInvertModule.cpp
#include "internal/cuda/CudaInvertModule.h"

#include "internal/cuda/CudaInvertKernels.h"
#include "internal/ProcessingModuleRegistry.h"

namespace scopeone::core::internal::cuda
{
    bool CudaInvertModule::processDevice(const GpuImage& input, GpuImage& output, int bitDepth)
    {
        return detail::launchInvert(input.data(), input.pitchBytes(), output.data(), output.pitchBytes(),
                                    input.width(), input.height(), bitDepth >= 16 ? 65535.0f : 255.0f);
    }
}

SCOPEONE_REGISTER_PROCESSING_MODULE(scopeone::core::internal::cuda::CudaInvertModule,
                                    QStringLiteral("cuda.invert"), QStringLiteral("Invert (CUDA)"), 1, {})
```

```cpp
// internal/cuda/CudaInvertKernels.h
#pragma once

#include <cstddef>

namespace scopeone::core::internal::cuda::detail
{
    bool launchInvert(const void* input, std::size_t inputPitch, void* output, std::size_t outputPitch,
                      int width, int height, float maxValue);
}
```

```cpp
// src/cuda/CudaInvertKernels.cu
#include "internal/cuda/CudaInvertKernels.h"
#include "internal/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>

namespace
{
    __global__ void invertKernel(const float* input, std::size_t inputPitch, float* output,
                                 std::size_t outputPitch, int width, int height, float maxValue)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }
        const auto* in = reinterpret_cast<const float*>(reinterpret_cast<const char*>(input) + y * inputPitch);
        auto* out = reinterpret_cast<float*>(reinterpret_cast<char*>(output) + y * outputPitch);
        out[x] = maxValue - in[x];
    }
}

namespace scopeone::core::internal::cuda::detail
{
    bool launchInvert(const void* input, std::size_t inputPitch, void* output, std::size_t outputPitch,
                      int width, int height, float maxValue)
    {
        const dim3 block(16, 16);
        const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
        invertKernel<<<grid, block>>>(static_cast<const float*>(input), inputPitch,
                                      static_cast<float*>(output), outputPitch, width, height, maxValue);
        return synchronizeKernel();
    }
}
```

The four files are the whole change.

- The kernels are launched through plain functions declared in the module's own kernels header, which has no Qt and no CUDA types, so the module sources compile without nvcc and no shared file changes. `CudaKernelLaunch.h` holds what the kernel files share, such as `synchronizeKernel`.
- The frames are pitched 2D float images by default. `GpuMemoryLayout::Contiguous` in the constructor gives a contiguous buffer for libraries that need one, such as cuFFT.
- The output has the size of the input. A method that bins, crops or scales overrides `outputSize` and gets an output image of that size.
- The output pixel is `value * scale + offset`, clamped to the processing bit depth, with a scale of 1 and an offset of 0 by default. A method with a signed result, such as a difference of Gaussians or a background subtraction, overrides `outputMapping` and returns an offset of half the maximum value, so zero lands in the middle of the range.
- cuFFT is linked. `CudaFrequencyFilterModule` shows how a module keeps its FFT plans and scratch buffers and recreates them when the frame size changes.
- NPP comes with the CUDA Toolkit. A module that uses it adds the library it needs, such as `CUDA::nppif` for filters, to `target_link_libraries` beside `CUDA::cufft`, and its DLL to the `_cuda_runtime_dlls` glob for the Windows package. NPP takes the frame's pointer, pitch and size directly.
- A failing CUDA call returns `false` from `processDevice`, and the module reports "CUDA processing failed" for that frame.

## Processing Data Flow

`ImageFrame` is the frame model used by preview, processing, recording, gallery and the local API. Processing recipes persist stable module IDs rather than registry positions. Available modules and their parameter descriptors come from the processing registry. Use `processFrameThrough(...)` to stop at one pipeline stage and `processFrameFrom(...)` to continue from a later module after an edited frame is written back. Saved OME-TIFF, OME-Zarr, TIFF and binary recording outputs are read back asynchronously through `ScopeOneCore::requestRecordingSessionFrame(...)`.

Real-time processing can consume all camera streams or one camera selected with `setRealTimeProcessingSource(...)`. `requestImageProcessing(...)` applies an isolated pipeline to one current image. `requestRecordingSessionStackProcessing(...)` applies one stateful isolated runtime to a complete session camera stack, reports progress, supports cancellation and creates a new in-memory Gallery session. These offline paths do not change live module buffers. The Local API can list, open, activate, process, save and close independent image windows backed by retained sessions.

Raw live frames, processed live frames, static tool/gallery frames, external API frames and session frame sources are routed through the core frame graph. UI preview widgets keep only a render cache, and callers should use `ScopeOneCore` frame facade methods instead of reading camera managers, recording sessions or preview cache state directly.

`ExperimentPlan` is the single recording and MDA input contract. `ExperimentDocument` adds actual event results, software and device provenance, output files, stable image layers, pixel-to-sensor transforms and markups. Schema version 2 documents include the sample-plane `pixelSizeUm` calibration, are validated strictly, and can be round-tripped with `experimentDocumentToJson(...)`, `saveExperimentDocument(...)` and `loadExperimentDocument(...)`; OME-TIFF is the default disk format. All OME-TIFF, OME-Zarr, plain TIFF and binary frame writing is delegated to the reusable ScopeWriter library. ScopeOne retains acquisition orchestration, experiment documents, output naming and reading.
