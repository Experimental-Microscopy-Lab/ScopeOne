add_library(ScopeOneNIDaqmx MODULE
    NIDaqmxPlugin.cpp
    NIDaqmxPlugin.h
    plugin.json
)
set_target_properties(ScopeOneNIDaqmx PROPERTIES OUTPUT_NAME "NIDaqmxPlugin")
target_link_libraries(ScopeOneNIDaqmx PRIVATE
    scopeone::PluginSDK
    Qt6::Core
)
scopeone_add_plugin(ScopeOneNIDaqmx hardware)
