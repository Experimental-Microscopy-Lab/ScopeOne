cmake_minimum_required(VERSION 3.16)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" SCOPEONE_VERSION LIMIT_COUNT 1)
project(ScopeOneCore VERSION "${SCOPEONE_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install")
if (WIN32)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.11.0/msvc2022_64")
endif ()

set(MMCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mmCoreAndDevices")
set(MMCORE_INCLUDE_DIR "${MMCORE_DIR}/MMCore")
set(MMDEVICE_INCLUDE_DIR "${MMCORE_DIR}/MMDevice")

find_package(Qt6 COMPONENTS Core Gui Network Concurrent REQUIRED)

if (WIN32)
    set(MMCORE_BIN_DIR "${MMCORE_DIR}/build/Release/x64")

    set(OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv-4.12.0")
    set(OpenCV_DIR "${OPENCV_DIR}/build")
    set(OPENCV_BIN_DIR "${OPENCV_DIR}/build/x64/vc16/bin")
    set(ZLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib-1.3.1")
    set(ZLIB_BIN_DIR "${ZLIB_DIR}/build/Release")
    set(ZLIB_INCLUDE_DIR "${ZLIB_DIR}" "${ZLIB_DIR}/build")
    set(ZLIB_LIB "${ZLIB_BIN_DIR}/zlib.lib")

    set(TIFF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tiff-4.7.1")
    set(TIFF_BIN_DIR "${TIFF_DIR}/build/libtiff/Release")
    set(TIFF_INCLUDE_DIRS "${TIFF_DIR}/libtiff;${TIFF_DIR}/build/libtiff")
    set(TIFF_LIB "${TIFF_BIN_DIR}/tiff.lib")

    find_package(OpenCV REQUIRED)

    file(GLOB OPENCV_RUNTIME_DLLS
            "${OPENCV_BIN_DIR}/opencv_world*.dll"
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

    foreach (_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent)
        get_target_property(_qt_runtime_dll ${_qt_target} IMPORTED_LOCATION_RELEASE)
        if (_qt_runtime_dll)
            list(APPEND SCOPEONE_RUNTIME_DLLS "${_qt_runtime_dll}")
        endif ()
    endforeach ()
else ()
    # Linux/Unix: use system OpenCV/TIFF/ZLIB, and link the MMCore static libraries
    # built from the sibling micro-manager checkout (symlinked into
    # external/mmCoreAndDevices). No runtime DLLs to stage.
    find_package(OpenCV REQUIRED COMPONENTS core imgproc)
    find_package(TIFF REQUIRED)
    find_package(ZLIB REQUIRED)

    set(TIFF_LIB TIFF::TIFF)
    set(TIFF_INCLUDE_DIRS "")
    set(ZLIB_LIB ZLIB::ZLIB)
    set(ZLIB_INCLUDE_DIR "")
    set(MMCORE_BIN_DIR "")

    add_library(MMCore STATIC IMPORTED GLOBAL)
    set_target_properties(MMCore PROPERTIES
            IMPORTED_LOCATION "${MMCORE_DIR}/MMCore/.libs/libMMCore.a"
            INTERFACE_INCLUDE_DIRECTORIES "${MMCORE_INCLUDE_DIR};${MMDEVICE_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};pthread"
    )

    set(SCOPEONE_RUNTIME_DLLS "")
endif ()

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
        src/ExperimentDocument.cpp
        src/ImageSceneModel.cpp
        src/MMCoreManager.cpp
        src/FrameBufferUtils.cpp
        src/ParticleAnalysis.cpp
        src/StageMosaicManager.cpp
        src/ImageProcessingFramework.cpp
        src/SpatiotemporalBinningModule.cpp
        src/GaussianBlurModule.cpp
        src/FFTModule.cpp
        src/BackgroundCalibrationModule.cpp
        src/DifferentialRollingModule.cpp
        src/MDAManager.cpp
        src/RecordingManager.cpp
        src/CameraBackend.cpp
        src/CameraManager.cpp
        src/NativeCameraBackend.cpp
        src/AgentCameraBackend.cpp
        src/ScopeOneCore.cpp
)

set(CORE_HEADERS
        include/scopeone/ExperimentDocument.h
        include/scopeone/ImageSceneModel.h
        internal/MMCoreManager.h
        internal/FrameBufferUtils.h
        internal/ParticleAnalysis.h
        internal/StageMosaicManager.h
        internal/ImageProcessingFramework.h
        internal/ProcessingModule.h
        internal/AgentProtocol.h
        internal/SpatiotemporalBinningModule.h
        internal/GaussianBlurModule.h
        internal/FFTModule.h
        internal/BackgroundCalibrationModule.h
        internal/DifferentialRollingModule.h
        internal/MDAManager.h
        internal/RecordingManager.h
        internal/CameraBackend.h
        internal/CameraManager.h
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
target_precompile_headers(ScopeOneCore PRIVATE
        <QtGlobal>
        <QObject>
        <QString>
        <QStringList>
        <QByteArray>
        <QVector>
        <QList>
        <QHash>
        <QJsonObject>
        <QJsonArray>
        <QJsonDocument>
        <QDateTime>
        <QElapsedTimer>
        <QTimer>
        <QThread>
        <QMetaObject>
        <QDebug>
        <algorithm>
        <atomic>
        <cmath>
        <cstring>
        <functional>
        <limits>
        <memory>
        <utility>
        <vector>
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

# Fast Release build
# if (MSVC)
#     target_compile_options(ScopeOneCore PRIVATE
#             $<$<CONFIG:Release>:/MP>
#     )
# endif ()

# LTO Release build
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

if (SCOPEONE_RUNTIME_DLLS)
    install(FILES ${SCOPEONE_RUNTIME_DLLS}
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif ()
