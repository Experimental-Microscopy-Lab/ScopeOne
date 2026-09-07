add_library(ScopeOneIscatProcessing MODULE
    IscatProcessingPlugin.cpp
    IscatProcessingModule.cpp
    IscatProcessingModule.h
    plugin.json
)

target_compile_features(ScopeOneIscatProcessing PRIVATE cxx_std_20)
target_link_libraries(ScopeOneIscatProcessing PRIVATE
    scopeone::PluginSDK
    opencv_core
    opencv_imgproc
    Qt6::Core
)
set_target_properties(ScopeOneIscatProcessing PROPERTIES
    OUTPUT_NAME "IscatProcessingPlugin"
)
scopeone_add_plugin(ScopeOneIscatProcessing processing)
