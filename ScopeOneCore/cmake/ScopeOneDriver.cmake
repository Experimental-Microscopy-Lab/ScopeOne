# Helpers for building ScopeOne drivers against an installed ScopeOneCore.
#
#   find_package(ScopeOneCore CONFIG REQUIRED)
#   add_library(MyCamera MODULE MyCamera.cpp driver.json)
#   target_link_libraries(MyCamera PRIVATE scopeone::DriverSDK)
#   scopeone_add_driver(MyCamera)
#
# The driver source names the generated manifest:
#   Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")

# Writes the generated manifest from the manifest beside the target's CMakeLists, adding the
# ScopeOneCore version the target is built against. ScopeOne loads only drivers built against its
# own Core, because they share C++ types with it.
function(scopeone_write_manifest target source generated)
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/${generated}")
    file(READ "${_source}" _manifest)
    string(JSON _manifest SET "${_manifest}" scopeOneCore "\"${ScopeOneCore_VERSION}\"")
    file(CONFIGURE OUTPUT "${_generated}" CONTENT "${_manifest}\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# Builds the target as a driver, from driver.json to scopeone_driver.json, and installs it in the
# drivers directory where ScopeOne looks for drivers
function(scopeone_add_driver target)
    scopeone_write_manifest(${target} driver.json scopeone_driver.json)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/drivers"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/drivers"
    )
    # A driver links only what it names. Linux leaves undefined symbols of a module to load time,
    # so a driver calling into Core past its SDK would link and fail only once loaded.
    if (UNIX AND NOT APPLE)
        target_link_options(${target} PRIVATE "LINKER:--no-undefined")
    endif ()
    install(TARGETS ${target}
        RUNTIME DESTINATION "drivers"
        LIBRARY DESTINATION "drivers"
    )
endfunction()
