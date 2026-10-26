add_library(ScopeOneBrillouinTool MODULE
    BrillouinToolPlugin.cpp
    plugin.json
)
target_compile_features(ScopeOneBrillouinTool PRIVATE cxx_std_20)
target_link_libraries(ScopeOneBrillouinTool PRIVATE
    scopeone::PluginSDK
    opencv_core
    Qt6::Widgets
)
scopeone_add_plugin(ScopeOneBrillouinTool tools)
