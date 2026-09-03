add_library(ScopeOneFrequencyFilter MODULE
    CudaFrequencyFilterModule.cpp
    CudaFrequencyFilterModule.h
    FrequencyFilterPlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneFrequencyFilter PRIVATE
    scopeone::GpuCuda
    scopeone::PluginSDK
    Qt6::Core
)
set_target_properties(ScopeOneFrequencyFilter PROPERTIES OUTPUT_NAME "FrequencyFilterPlugin")
scopeone_add_plugin(ScopeOneFrequencyFilter processing)
