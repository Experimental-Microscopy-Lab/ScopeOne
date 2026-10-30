# Helpers for building ScopeOne plugins against an installed ScopeOneCore.
#
#   find_package(ScopeOneCore CONFIG REQUIRED)
#   add_library(MyPlugin MODULE MyPlugin.cpp plugin.json)
#   target_link_libraries(MyPlugin PRIVATE scopeone::PluginSDK)
#   scopeone_add_plugin(MyPlugin processing)
#
# The plugin source names the generated manifest:
#   Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "scopeone_plugin.json")

# Writes scopeone_plugin.json from the plugin.json beside the target's CMakeLists, adding the
# ScopeOneCore version the plugin is built against. ScopeOne loads only plugins built against
# its own Core, because plugins share C++ types with it.
function(scopeone_plugin_manifest target)
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/scopeone_plugin.json")
    file(READ "${_source}" _manifest)
    string(JSON _manifest SET "${_manifest}" scopeOneCore "\"${ScopeOneCore_VERSION}\"")
    file(CONFIGURE OUTPUT "${_generated}" CONTENT "${_manifest}\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# Builds the target as a plugin of the given kind (processing, tools or hardware) and installs
# it where ScopeOne looks for that kind
function(scopeone_add_plugin target kind)
    scopeone_plugin_manifest(${target})
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${kind}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${kind}"
    )
    install(TARGETS ${target}
        RUNTIME DESTINATION "plugins/${kind}"
        LIBRARY DESTINATION "plugins/${kind}"
    )
endfunction()
