add_library(ScopeOneSimulatedPmt MODULE
    SimulatedPmtPlugin.cpp
    SimulatedPmtPlugin.h
    SimulatedPmtSource.cpp
    SimulatedPmtSource.h
    plugin.json
)

target_link_libraries(ScopeOneSimulatedPmt PRIVATE
    scopeone::PluginSDK
    Qt6::Core
)

scopeone_add_plugin(ScopeOneSimulatedPmt hardware)
