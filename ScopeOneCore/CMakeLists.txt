cmake_minimum_required(VERSION 3.16)
project(ScopeOneCore VERSION 2.0.405 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(SCOPEONE_BUILD_PYTHON_BINDINGS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install")
set(CMAKE_PREFIX_PATH "C:/Qt/6.11.0/msvc2022_64")

set(PYBIND11_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/pybind11-3.0.1")

set(MMCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mmCoreAndDevices")
set(MMCORE_INCLUDE_DIR "${MMCORE_DIR}/MMCore")
set(MMDEVICE_INCLUDE_DIR "${MMCORE_DIR}/MMDevice")
set(MMCORE_BIN_DIR "${MMCORE_DIR}/build/Release/x64")

set(OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv-4.12.0")
set(OpenCV_DIR "${OPENCV_DIR}/build")
set(OPENCV_BIN_DIR "${OPENCV_DIR}/build/x64/vc16/bin")
set(ZLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib-1.3.1")
set(ZLIB_BIN_DIR "${ZLIB_DIR}/build/Release")
set(ZLIB_INCLUDE_DIR "${ZLIB_DIR}")
set(ZLIB_LIB "${ZLIB_BIN_DIR}/zlib.lib")

set(TIFF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tiff-4.7.1")
set(TIFF_BIN_DIR "${TIFF_DIR}/build/libtiff/Release")
set(TIFF_INCLUDE_DIRS "${TIFF_DIR}/libtiff;${TIFF_DIR}/build/libtiff")
set(TIFF_LIB "${TIFF_BIN_DIR}/tiff.lib")

find_package(Qt6 COMPONENTS Core Gui Network Concurrent REQUIRED)
find_package(OpenCV REQUIRED)

file(GLOB OPENCV_RUNTIME_DLLS
        "${OPENCV_BIN_DIR}/opencv_world[0-9]*[0-9].dll"
        "${OPENCV_BIN_DIR}/opencv_videoio_ffmpeg*_64.dll"
)
file(GLOB MMCORE_RUNTIME_DLLS "${MMCORE_BIN_DIR}/*.dll")
set(TIFF_RUNTIME_DLL "${TIFF_BIN_DIR}/tiff.dll")
set(ZLIB_RUNTIME_DLL "${ZLIB_BIN_DIR}/zlib.dll")

set(SCOPEONE_RUNTIME_DLLS
        ${OPENCV_RUNTIME_DLLS}
        "${TIFF_RUNTIME_DLL}"
        "${ZLIB_RUNTIME_DLL}"
        ${MMCORE_RUNTIME_DLLS}
)

function(scopeone_add_runtime_copy_commands target destination)
    foreach (_dll IN LISTS SCOPEONE_RUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}"
                "${destination}"
        )
    endforeach ()
endfunction()

set(CORE_SOURCES
        src/MMCoreManager.cpp
        src/FrameBufferUtils.cpp
        src/ImageProcessingFramework.cpp
        src/ProcessingModule.cpp
        src/SpatiotemporalBinningModule.cpp
        src/GaussianBlurModule.cpp
        src/FFTModule.cpp
        src/MedianFilterModule.cpp
        src/BackgroundCalibrationModule.cpp
        src/ProcessingModuleTemplate.cpp
        src/MDAManager.cpp
        src/RecordingManager.cpp
        src/MultiProcessCameraManager.cpp
        src/ScopeOneCore.cpp
)

set(CORE_HEADERS
        internal/MMCoreManager.h
        internal/FrameBufferUtils.h
        internal/ImageProcessingFramework.h
        internal/ProcessingModule.h
        internal/ProcessingModuleTemplate.h
        internal/AgentProtocol.h
        internal/SpatiotemporalBinningModule.h
        internal/GaussianBlurModule.h
        internal/FFTModule.h
        internal/MedianFilterModule.h
        internal/BackgroundCalibrationModule.h
        internal/MDAManager.h
        internal/RecordingManager.h
        internal/MultiProcessCameraManager.h
        include/scopeone/ImageFrame.h
        include/scopeone/SharedFrame.h
        include/scopeone/scopeone_core_export.h
        include/scopeone/ScopeOneCore.h
)

add_library(ScopeOneCore SHARED ${CORE_SOURCES} ${CORE_HEADERS})
add_library(scopeone::ScopeOneCore ALIAS ScopeOneCore)
target_link_libraries(ScopeOneCore
        PUBLIC
        Qt::Core
        Qt::Gui
        PRIVATE
        MMCore
        Qt::Network
        Qt::Concurrent
        ${OpenCV_LIBS}
        ${TIFF_LIB}
        ${ZLIB_LIB}
)
target_compile_definitions(ScopeOneCore PRIVATE
        MMDEVICE_CLIENT_BUILD
        SCOPEONE_HAVE_TIFF
        SCOPEONE_CORE_EXPORTS
        SCOPEONE_CORE_VERSION_STRING="${PROJECT_VERSION}"
)
target_include_directories(ScopeOneCore
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MMCORE_INCLUDE_DIR}
        ${MMDEVICE_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
        ${TIFF_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIR}
)
target_link_directories(ScopeOneCore PRIVATE ${MMCORE_BIN_DIR})

scopeone_add_runtime_copy_commands(ScopeOneCore $<TARGET_FILE_DIR:ScopeOneCore>)

add_executable(ScopeOne_Agent
        src/AgentMain.cpp
)
target_link_libraries(ScopeOne_Agent
        MMCore
        Qt::Core
        Qt::Network
)
target_compile_definitions(ScopeOne_Agent PRIVATE MMDEVICE_CLIENT_BUILD)
target_include_directories(ScopeOne_Agent PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MMCORE_INCLUDE_DIR}
        ${MMDEVICE_INCLUDE_DIR}
)
target_link_directories(ScopeOne_Agent PRIVATE ${MMCORE_BIN_DIR})

scopeone_add_runtime_copy_commands(ScopeOne_Agent $<TARGET_FILE_DIR:ScopeOne_Agent>)

if (SCOPEONE_BUILD_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
    add_subdirectory("${PYBIND11_DIR}" "${CMAKE_BINARY_DIR}/_deps/pybind11" EXCLUDE_FROM_ALL)

    pybind11_add_module(pyscopeonecore MODULE
            python/ScopeOneCoreBindings.cpp
    )
    target_link_libraries(pyscopeonecore PRIVATE scopeone::ScopeOneCore)
    set_target_properties(pyscopeonecore PROPERTIES
            OUTPUT_NAME "_core"
            LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/python/scopeone/src/scopeone>"
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/python/scopeone/src/scopeone>"
    )
    add_custom_command(TARGET pyscopeonecore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ScopeOneCore>
            $<TARGET_FILE_DIR:pyscopeonecore>
    )
    scopeone_add_runtime_copy_commands(pyscopeonecore $<TARGET_FILE_DIR:pyscopeonecore>)

    install(TARGETS pyscopeonecore
            LIBRARY DESTINATION "python/scopeone/src/scopeone"
            RUNTIME DESTINATION "python/scopeone/src/scopeone"
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/scopeone/"
            DESTINATION "python/scopeone"
            FILES_MATCHING
            PATTERN "*.py"
    )
endif ()

if (MSVC)
    target_compile_options(ScopeOneCore PRIVATE
            $<$<CONFIG:Release>:/O2 /GL /fp:fast /arch:AVX2>
    )
    target_compile_definitions(ScopeOneCore PRIVATE
            $<$<CONFIG:Release>:NDEBUG>
    )
    target_link_options(ScopeOneCore PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )

    target_compile_options(ScopeOne_Agent PRIVATE
            $<$<CONFIG:Release>:/O2 /GL /fp:fast /arch:AVX2>
    )
    target_compile_definitions(ScopeOne_Agent PRIVATE
            $<$<CONFIG:Release>:NDEBUG>
    )
    target_link_options(ScopeOne_Agent PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )

    if (TARGET pyscopeonecore)
        target_compile_options(pyscopeonecore PRIVATE
                $<$<CONFIG:Release>:/O2 /GL /fp:fast /arch:AVX2>
        )
        target_compile_definitions(pyscopeonecore PRIVATE
                $<$<CONFIG:Release>:NDEBUG>
        )
    endif ()
endif ()

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScopeOneCoreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCore"
)
export(TARGETS ScopeOneCore
        FILE "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreTargets.cmake"
        NAMESPACE scopeone::
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
)

install(TARGETS ScopeOneCore
        EXPORT ScopeOneCoreTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(TARGETS ScopeOne_Agent
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING
        PATTERN "*.h"
)
install(EXPORT ScopeOneCoreTargets
        FILE ScopeOneCoreTargets.cmake
        NAMESPACE scopeone::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCore"
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCore"
)

install(FILES ${SCOPEONE_RUNTIME_DLLS}
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
