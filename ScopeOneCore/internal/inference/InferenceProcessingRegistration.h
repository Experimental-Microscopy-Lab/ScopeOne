#pragma once

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry;
}

namespace scopeone::core::internal::inference
{
    // Registers the ONNX Runtime processing modules, built into Core when SCOPEONE_ENABLE_INFERENCE is on
    void registerInferenceProcessingModules(core::internal::ProcessingModuleRegistry& registry);
}
