cmake_minimum_required(VERSION 3.23)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ScopeOneInference VERSION 1.0.0 LANGUAGES CXX)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    find_package(ScopeOneCore CONFIG REQUIRED)
endif ()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_AUTOMOC ON)

set(ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../ScopeOneCore/external/onnxruntime-win-x64-gpu_cuda12-1.30.0"
    CACHE PATH "ONNX Runtime SDK root"
)

find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/include"
    NO_DEFAULT_PATH
    REQUIRED
)
find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    PATHS "${ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH
    REQUIRED
)

add_library(ScopeOneInference SHARED
    src/OnnxInternal.h
    src/OnnxRuntime.cpp
    src/OnnxSession.cpp
    src/OnnxInferenceModule.cpp
    src/OnnxInferenceModule.h
    src/InferenceProcessingRegistration.cpp
    plugin.json
    include/scopeone/inference/ExecutionProvider.h
    include/scopeone/inference/InferenceExport.h
    include/scopeone/inference/OnnxRuntime.h
    include/scopeone/inference/OnnxSession.h
    include/scopeone/inference/TensorInfo.h
)
add_library(scopeone::Inference ALIAS ScopeOneInference)

set_target_properties(ScopeOneInference PROPERTIES
    EXPORT_NAME Inference
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/processing"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/processing"
)
target_compile_features(ScopeOneInference PUBLIC cxx_std_20)
target_compile_definitions(ScopeOneInference PRIVATE SCOPEONE_INFERENCE_EXPORTS)
target_include_directories(ScopeOneInference
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${ONNXRUNTIME_INCLUDE_DIR}"
)
target_link_libraries(ScopeOneInference PRIVATE
    "${ONNXRUNTIME_LIBRARY}"
    scopeone::ScopeOneCore
    Qt6::Core
)

if (WIN32)
    file(GLOB ONNXRUNTIME_DLLS
        "${ONNXRUNTIME_ROOT}/bin/onnxruntime*.dll"
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime*.dll"
    )
    if (NOT ONNXRUNTIME_DLLS)
        message(FATAL_ERROR "No ONNX Runtime DLLs found under ${ONNXRUNTIME_ROOT}")
    endif ()
    add_custom_command(TARGET ScopeOneInference POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ONNXRUNTIME_DLLS}
            "${CMAKE_BINARY_DIR}/plugins/processing"
        COMMAND_EXPAND_LISTS
    )
endif ()

install(TARGETS ScopeOneInference
    EXPORT ScopeOneInferenceTargets
    RUNTIME DESTINATION "plugins/processing"
    LIBRARY DESTINATION "plugins/processing"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if (ONNXRUNTIME_DLLS)
    install(FILES ${ONNXRUNTIME_DLLS} DESTINATION "plugins/processing")
endif ()
install(EXPORT ScopeOneInferenceTargets
    FILE ScopeOneInferenceTargets.cmake
    NAMESPACE scopeone::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneInference"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScopeOneInferenceConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneInferenceConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneInference"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneInferenceConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneInferenceConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneInferenceConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneInference"
)
