cmake_minimum_required(VERSION 3.23)
include("${CMAKE_CURRENT_LIST_DIR}/../ScopeOneCore/cmake/ScopeOneQt.cmake")
project(ScopeOneDrivers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

# Xcode's libc++ hides std::jthread and std::stop_token unless the experimental library is enabled
if (APPLE)
    add_compile_options(-fexperimental-library)
    add_link_options(-fexperimental-library)
endif ()
# The Visual Studio generator compiles the sources of one project one after another unless the
# compiler runs them in parallel itself
if (MSVC)
    add_compile_options(/MP)
endif ()

# A driver has the driver SDK and Qt Core and nothing else, so one that reaches for widgets,
# OpenCV or Core itself does not build
find_package(Qt6 ${SCOPEONE_QT_MINIMUM_VERSION} REQUIRED COMPONENTS Core)
find_package(ScopeOneCore CONFIG REQUIRED)

add_subdirectory(ExampleCamera)
add_subdirectory(ExamplePmt)
add_subdirectory(NIDaqmx)
