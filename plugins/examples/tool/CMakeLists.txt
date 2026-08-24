add_library(ScopeOneExampleTool MODULE
    ExampleToolPlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneExampleTool PRIVATE scopeone::PluginSDK Qt6::Widgets)
scopeone_example_plugin(ScopeOneExampleTool tools)
