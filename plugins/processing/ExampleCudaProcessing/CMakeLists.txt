add_library(ScopeOneExampleCudaProcessing MODULE
    ExampleCudaProcessingPlugin.cpp
    ExampleCudaKernel.cu
    plugin.json
)
target_link_libraries(ScopeOneExampleCudaProcessing PRIVATE
    scopeone::Cuda
    scopeone::PluginSDK
    Qt6::Core
    CUDA::cudart
)
set_target_properties(ScopeOneExampleCudaProcessing PROPERTIES
    CUDA_ARCHITECTURES "${SCOPEONE_CUDA_ARCHITECTURES}"
)
set_target_properties(ScopeOneExampleCudaProcessing PROPERTIES OUTPUT_NAME "ExampleCudaProcessing")
scopeone_add_plugin(ScopeOneExampleCudaProcessing processing)
