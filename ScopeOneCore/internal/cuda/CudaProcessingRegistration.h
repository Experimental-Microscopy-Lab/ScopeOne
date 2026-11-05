#pragma once

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry;
}

namespace scopeone::cuda
{
    // Registers the CUDA processing modules, built into Core when SCOPEONE_ENABLE_CUDA is on
    void registerCudaProcessingModules(core::internal::ProcessingModuleRegistry& registry);
}
