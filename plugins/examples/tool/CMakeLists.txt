add_library(ScopeOneExampleTool MODULE
    ExampleToolPlugin.cpp
    plugin.json
)
target_link_libraries(ScopeOneExampleTool PRIVATE scopeone::PluginSDK Qt6::Widgets)
scopeone_example_plugin(ScopeOneExampleTool tools)

add_library(ScopeOneDhmTool MODULE
    DhmToolPlugin.cpp
    DhmReconstruction.cpp
    DhmReconstruction.h
    dhm_plugin.json
)
target_compile_features(ScopeOneDhmTool PRIVATE cxx_std_20)
target_link_libraries(ScopeOneDhmTool PRIVATE
    scopeone::PluginSDK
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    Qt6::Widgets
)
scopeone_example_plugin(ScopeOneDhmTool tools)
