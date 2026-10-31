add_library(ScopeOnePythonProcessing MODULE
    PythonProcessingPlugin.cpp
    plugin.json
)
target_compile_features(ScopeOnePythonProcessing PRIVATE cxx_std_20)
target_link_libraries(ScopeOnePythonProcessing PRIVATE scopeone::PluginSDK Qt6::Core)
scopeone_add_plugin(ScopeOnePythonProcessing processing)
