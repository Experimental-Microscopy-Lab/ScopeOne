add_library(ScopeOneExampleProcessing MODULE
    ExampleProcessingPlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneExampleProcessing PRIVATE scopeone::PluginSDK Qt6::Core)
scopeone_add_plugin(ScopeOneExampleProcessing processing)
