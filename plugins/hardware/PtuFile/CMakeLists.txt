add_library(ScopeOnePtuFile MODULE
    PtuFilePlugin.cpp
    PtuFilePlugin.h
    plugin.json
)
set_target_properties(ScopeOnePtuFile PROPERTIES OUTPUT_NAME "PtuFilePlugin")
target_link_libraries(ScopeOnePtuFile PRIVATE
    scopeone::PluginSDK
    Qt6::Core
)
scopeone_add_plugin(ScopeOnePtuFile hardware)
