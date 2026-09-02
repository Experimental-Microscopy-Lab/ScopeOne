add_library(ScopeOneScanningDaqTool MODULE
    DaqControlWidget.cpp
    DaqControlWidget.h
    SignalMonitorWidget.cpp
    SignalMonitorWidget.h
    ScanningDaqToolPlugin.cpp
    plugin.json
)
set_target_properties(ScopeOneScanningDaqTool PROPERTIES OUTPUT_NAME "ScanningDaqTool")
target_link_libraries(ScopeOneScanningDaqTool PRIVATE
    scopeone::PluginSDK
    Qt6::Widgets
)
scopeone_add_plugin(ScopeOneScanningDaqTool tools)
