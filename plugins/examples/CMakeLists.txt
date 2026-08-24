cmake_minimum_required(VERSION 3.23)
project(ScopeOnePluginExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(ScopeOneCore CONFIG REQUIRED)

function(scopeone_example_plugin target kind)
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

add_subdirectory(processing)
add_subdirectory(tool)
add_subdirectory(hardware)
