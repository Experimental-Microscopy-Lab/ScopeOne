# ScopeOne plugin examples

These projects use the installed `scopeone::PluginSDK` target and do not depend on ScopeOne source files. The public headers live in `ScopeOneCore/include/scopeone`; include `scopeone/PluginSDK.h` as the SDK entry point. The same plugin build also contains the external NI-DAQmx, PTU and scanning tool plugins.

The normal ScopeOne build script builds and installs these examples automatically during a full build:

```powershell
.\scripts\build.ps1
```

To build only the examples:

```powershell
.\scripts\build.ps1 --target plugins
```

Each example plugin declares `id`, `name`, `version`, `scopeOneApi`, and `kind` in `plugin.json`. Processing plugins implement `ProcessingPlugin` and add modules to the shared pipeline. Hardware providers implement `DriverHostProviderPlugin` and run in `ScopeOne_DriverHost`. DAQ plugins implement `DaqDevicePlugin`, signal sources implement `SignalSourcePlugin`, and tool plugins implement `ScopeOneToolPlugin`.

Install processing plugins under `plugins/processing`, tool plugins under `plugins/tools`, and hardware, DAQ and signal source plugins under `plugins/hardware`.

Tool plugins can create an independent processing pipeline without changing the main Process panel:

```cpp
auto pipeline = context.core().createProcessingPipeline();
pipeline->addModule(context.core().createProcessingModule("fft"));
pipeline->addModule(context.core().createProcessingModule("mask"));
pipeline->addModule(context.core().createProcessingModule("ifft"));
auto result = pipeline->process(scopeone::core::ProcessingValue{inputFrame});
```

The pipeline accepts image and complex intermediate values. Publish image results with `publishExternalFrame` and show the returned layer through the tool context.
