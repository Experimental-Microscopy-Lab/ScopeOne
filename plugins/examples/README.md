# ScopeOne plugin examples

These projects use the installed `scopeone::PluginSDK` target and do not depend on ScopeOne source files. Include `scopeone/PluginSDK.h` as the public SDK entry point.

The normal ScopeOne build script builds and installs these examples automatically during a full build:

```powershell
.\scripts\build.ps1
```

To build only the examples:

```powershell
.\scripts\build.ps1 --target plugins
```

Each example plugin declares `id`, `name`, `version`, `scopeOneApi`, and `kind` in `plugin.json`. Processing plugins implement `ProcessingPlugin` and add modules to the shared pipeline. Hardware providers implement `DriverHostProviderPlugin` and run in `ScopeOne_DriverHost`. Tool plugins implement `ScopeOneToolPlugin` and own an independent workflow window. `ExampleHardwarePlugin` wraps the public `SimulatorProvider` to demonstrate a complete external hardware plugin.

Install processing plugins under `plugins/processing`, tool plugins under `plugins/tools`, and hardware plugins under `plugins/hardware`.

Tool plugins can create an independent processing pipeline without changing the main Process panel:

```cpp
auto pipeline = context.core().createProcessingPipeline();
pipeline->addModule(context.core().createProcessingModule("fft"));
pipeline->addModule(context.core().createProcessingModule("mask"));
pipeline->addModule(context.core().createProcessingModule("ifft"));
auto result = pipeline->process(scopeone::core::ProcessingValue{inputFrame});
```

The pipeline accepts image and complex intermediate values. Publish image results with `publishExternalFrame` and show the returned layer through the tool context.
