add_library(ScopeOneExampleCamera MODULE
    ExampleCameraPlugin.cpp
    TestPatterns.cpp
    TestPatterns.h
    plugin.json
)
target_link_libraries(ScopeOneExampleCamera PRIVATE scopeone::HardwarePluginSDK Qt6::Core)
scopeone_add_plugin(ScopeOneExampleCamera hardware)
