add_library(ScopeOneDhmTool MODULE
    DhmToolPlugin.cpp
    DhmReconstruction.cpp
    DhmReconstruction.h
    plugin.json
)
target_compile_features(ScopeOneDhmTool PRIVATE cxx_std_20)
target_link_libraries(ScopeOneDhmTool PRIVATE
    scopeone::PluginSDK
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    Qt6::Widgets
)
scopeone_add_plugin(ScopeOneDhmTool tools)
