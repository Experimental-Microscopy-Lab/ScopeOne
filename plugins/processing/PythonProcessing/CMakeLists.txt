add_library(ScopeOnePythonProcessing MODULE
    PythonProcessingPlugin.cpp
    plugin.json
)
target_compile_features(ScopeOnePythonProcessing PRIVATE cxx_std_20)
target_link_libraries(ScopeOnePythonProcessing PRIVATE scopeone::PluginSDK Qt6::Core)
scopeone_add_plugin(ScopeOnePythonProcessing processing)

# Python modules sit in the python folder beside the processing plugins
configure_file(python/example_offset.py
    "${CMAKE_BINARY_DIR}/plugins/processing/python/example_offset.py" COPYONLY)
install(FILES python/example_offset.py DESTINATION "plugins/processing/python")
