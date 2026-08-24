add_library(ScopeOneExampleHardware MODULE
    ExampleHardwarePlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneExampleHardware PRIVATE scopeone::PluginSDK Qt6::Core)
scopeone_example_plugin(ScopeOneExampleHardware hardware)
