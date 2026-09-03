add_library(ScopeOneGaussianBlur MODULE
    CudaGaussianBlurModule.cpp
    CudaGaussianBlurModule.h
    GaussianBlurPlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneGaussianBlur PRIVATE
    scopeone::GpuCuda
    scopeone::PluginSDK
    Qt6::Core
)
set_target_properties(ScopeOneGaussianBlur PROPERTIES OUTPUT_NAME "GaussianBlurPlugin")
scopeone_add_plugin(ScopeOneGaussianBlur processing)
