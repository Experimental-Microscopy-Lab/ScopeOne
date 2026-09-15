cmake_minimum_required(VERSION 3.23)
project(ScopeOnePlugins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(ScopeOneCore CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)
find_program(SCOPEONE_CUDA_NVCC_EXECUTABLE nvcc)

function(scopeone_add_plugin target kind)
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

add_subdirectory(hardware)
add_subdirectory(tools)
if (SCOPEONE_CUDA_NVCC_EXECUTABLE)
    set(CMAKE_CUDA_COMPILER "${SCOPEONE_CUDA_NVCC_EXECUTABLE}" CACHE FILEPATH
        "CUDA compiler selected by the environment" FORCE)
    get_filename_component(_cuda_toolkit_root "${CMAKE_CUDA_COMPILER}" DIRECTORY)
    get_filename_component(_cuda_toolkit_root "${_cuda_toolkit_root}" DIRECTORY)
    set(CUDAToolkit_ROOT "${_cuda_toolkit_root}" CACHE PATH
        "CUDA toolkit selected by the compiler" FORCE)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../ScopeOneCuda ScopeOneCuda)
endif ()
add_subdirectory(processing)
