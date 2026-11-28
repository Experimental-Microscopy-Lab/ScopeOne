cmake_minimum_required(VERSION 3.23)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ScopeOneVersion.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ScopeOneQt.cmake")
scopeone_resolve_version(SCOPEONE_VERSION "${CMAKE_CURRENT_LIST_DIR}/..")
project(ScopeOneCore VERSION "${SCOPEONE_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif ()

set(MMCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mmCoreAndDevices")
set(MMCORE_INCLUDE_DIR "${MMCORE_DIR}/MMCore")
set(MMDEVICE_INCLUDE_DIR "${MMCORE_DIR}/MMDevice")

find_package(Qt6 ${SCOPEONE_QT_MINIMUM_VERSION} COMPONENTS Core Gui Network Concurrent REQUIRED)
message(STATUS "Using Qt ${Qt6_VERSION} from ${Qt6_DIR}")
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)
# pocketfft is one header, which vcpkg installs without a CMake package
find_path(POCKETFFT_INCLUDE_DIR pocketfft_hdronly.h REQUIRED)

set(SCOPEWRITER_BUILD_TESTS OFF)
add_subdirectory(external/ScopeWriter)

set(CMAKE_AUTOMOC ON)

if (WIN32)
    set(MMCORE_BIN_DIR "${MMCORE_DIR}/build/Release/x64")

    file(GLOB MMCORE_RUNTIME_DLLS "${MMCORE_BIN_DIR}/*.dll")
    file(GLOB VCPKG_RUNTIME_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")

    set(SCOPEONE_RUNTIME_DLLS
            ${VCPKG_RUNTIME_DLLS}
            ${MMCORE_RUNTIME_DLLS}
    )

    foreach (_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent)
        get_target_property(_qt_runtime_dll ${_qt_target} IMPORTED_LOCATION_RELEASE)
        if (_qt_runtime_dll)
            list(APPEND SCOPEONE_RUNTIME_DLLS "${_qt_runtime_dll}")
        endif ()
    endforeach ()
else ()
    # Linux/Unix: link the MMCore static libraries
    # built from the sibling micro-manager checkout (symlinked into
    # external/mmCoreAndDevices). No runtime DLLs to stage.
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

# Every source of Core is picked up from its folder, so a new file, such as a processing module that
# registers itself, needs no change here. DriverHostMain.cpp belongs to ScopeOne_DriverHost.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/DriverHostMain.cpp")
file(GLOB CORE_HEADERS CONFIGURE_DEPENDS internal/*.h include/scopeone/*.h)

# The driver SDK: the only headers and the only library a driver sees. It is a library of its
# own so a driver that reaches for anything else of Core does not compile or link.
set(SDK_HEADERS
        sdk/include/scopeone/Camera.h
        sdk/include/scopeone/DaqDevice.h
        sdk/include/scopeone/Driver.h
        sdk/include/scopeone/HardwareCapabilities.h
        sdk/include/scopeone/DriverSDK.h
        sdk/include/scopeone/Device.h
        sdk/include/scopeone/HardwareTypes.h
        sdk/include/scopeone/ImageFrame.h
        sdk/include/scopeone/scopeone_sdk_export.h
        sdk/include/scopeone/SignalSource.h
)
add_library(ScopeOneDriverSDK SHARED
        sdk/src/Device.cpp
        sdk/src/SignalSource.cpp
        ${SDK_HEADERS}
)
add_library(scopeone::DriverSDK ALIAS ScopeOneDriverSDK)
set_target_properties(ScopeOneDriverSDK PROPERTIES EXPORT_NAME DriverSDK)
target_link_libraries(ScopeOneDriverSDK PUBLIC Qt::Core)
target_compile_definitions(ScopeOneDriverSDK PRIVATE SCOPEONE_SDK_EXPORTS)
target_include_directories(ScopeOneDriverSDK PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sdk>
)

add_library(ScopeOneCore SHARED ${CORE_SOURCES} ${CORE_HEADERS})
add_library(scopeone::ScopeOneCore ALIAS ScopeOneCore)
# macOS has no $ORIGIN. Without an @loader_path entry the installed libraries and DriverHost had
# no LC_RPATH at all, so dyld could not resolve @rpath and the host died before loading a camera.
if (APPLE)
    set(SCOPEONE_RPATH_ORIGIN "@loader_path")
elseif (UNIX)
    set(SCOPEONE_RPATH_ORIGIN "$ORIGIN")
endif ()
if (UNIX)
    set_target_properties(ScopeWriter ScopeOneCore ScopeOneDriverSDK PROPERTIES
            BUILD_RPATH "${SCOPEONE_RPATH_ORIGIN}"
            INSTALL_RPATH "${SCOPEONE_RPATH_ORIGIN}"
            INSTALL_RPATH_USE_LINK_PATH TRUE
    )
endif ()
target_link_libraries(ScopeOneCore
        PUBLIC
        ScopeOneDriverSDK
        Qt::Core
        Qt::Gui
        PRIVATE
        MMCore
        Qt::Network
        Qt::Concurrent
        opencv_core
        opencv_imgproc
        opencv_imgcodecs
        ScopeWriter::ScopeWriter
)
target_compile_definitions(ScopeOneCore PRIVATE
        MMDEVICE_CLIENT_BUILD
        SCOPEONE_CORE_EXPORTS
        SCOPEONE_CORE_VERSION_STRING="${PROJECT_VERSION}"
)

# The CUDA processing modules are part of Core when nvcc is found, and turning them off builds the
# standard Core. ONNX inference is always built, from the ONNX Runtime package that matches CUDA.
set(_with_cuda OFF)
set(_with_inference OFF)
# Why a module that is turned on was left out, for the summary of the build scripts
set(_cuda_reason "")
set(_inference_reason "")
option(SCOPEONE_ENABLE_CUDA "Build the CUDA processing modules into Core when nvcc is available" ON)
if (SCOPEONE_ENABLE_CUDA)
    # An nvcc that was uninstalled since the last configure is looked up again
    if (SCOPEONE_CUDA_NVCC_EXECUTABLE AND NOT EXISTS "${SCOPEONE_CUDA_NVCC_EXECUTABLE}")
        unset(SCOPEONE_CUDA_NVCC_EXECUTABLE CACHE)
        unset(CMAKE_CUDA_COMPILER CACHE)
    endif ()
    find_program(SCOPEONE_CUDA_NVCC_EXECUTABLE nvcc)
    if (NOT SCOPEONE_CUDA_NVCC_EXECUTABLE)
        set(_cuda_reason "nvcc was not found on the path")
        message(STATUS "${_cuda_reason}; building Core without CUDA")
    endif ()
else ()
    unset(SCOPEONE_CUDA_NVCC_EXECUTABLE CACHE)
    set(_cuda_reason "turned off with SCOPEONE_ENABLE_CUDA")
endif ()
if (SCOPEONE_CUDA_NVCC_EXECUTABLE)
    # A CUDA that cannot build here, such as one installed before Visual Studio and so without its
    # MSBuild integration, or one too old for the MSVC version, left the configure failing.
    # check_language tries it in a separate project first, and CUDA is left out when it fails.
    if (NOT CMAKE_CUDA_COMPILER)
        set(ENV{CUDACXX} "${SCOPEONE_CUDA_NVCC_EXECUTABLE}")
        include(CheckLanguage)
        check_language(CUDA)
    endif ()
    if (CMAKE_CUDA_COMPILER)
        get_filename_component(_cuda_toolkit_root "${CMAKE_CUDA_COMPILER}" DIRECTORY)
        get_filename_component(_cuda_toolkit_root "${_cuda_toolkit_root}" DIRECTORY)
        set(CUDAToolkit_ROOT "${_cuda_toolkit_root}" CACHE PATH
                "CUDA toolkit selected by the compiler" FORCE)
        enable_language(CUDA)
        find_package(CUDAToolkit)
        if (NOT TARGET CUDA::cufft)
            set(_cuda_reason "the CUDA toolkit at ${_cuda_toolkit_root} has no cuFFT")
            message(STATUS "${_cuda_reason}; building Core without CUDA")
        endif ()
    else ()
        set(_cuda_reason "nvcc at ${SCOPEONE_CUDA_NVCC_EXECUTABLE} cannot build with this compiler, often a CUDA installed before Visual Studio or too old for it")
        message(STATUS "${_cuda_reason}; building Core without CUDA")
    endif ()
endif ()
if (SCOPEONE_CUDA_NVCC_EXECUTABLE AND TARGET CUDA::cufft)
    set(SCOPEONE_CUDA_ARCHITECTURES "50;75;80;86;89" CACHE STRING "Target CUDA architectures")
    file(GLOB _cuda_sources CONFIGURE_DEPENDS src/cuda/*.cpp internal/cuda/*.h)
    file(GLOB _cuda_kernels CONFIGURE_DEPENDS src/cuda/*.cu)
    target_sources(ScopeOneCore PRIVATE ${_cuda_sources} ${_cuda_kernels})
    set_source_files_properties(${_cuda_kernels} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_target_properties(ScopeOneCore PROPERTIES
            CUDA_ARCHITECTURES "${SCOPEONE_CUDA_ARCHITECTURES}"
            CUDA_SEPARABLE_COMPILATION OFF
    )
    target_link_libraries(ScopeOneCore PRIVATE CUDA::cudart CUDA::cufft)
    set(_with_cuda ON)
    if (WIN32)
        file(GLOB _cuda_runtime_dlls
                "${CUDAToolkit_BIN_DIR}/cudart64_*.dll"
                "${CUDAToolkit_BIN_DIR}/cufft64_*.dll"
        )
        list(APPEND SCOPEONE_RUNTIME_DLLS ${_cuda_runtime_dlls})
    endif ()
endif ()

# ONNX Runtime is the official prebuilt package from NuGet, downloaded into external on the first
# configure. The GitHub releases it came from before were deleted with the onnxruntime repository
# in November 2026, and Core silently built without ONNX inference. Core built with CUDA takes the
# GPU package and Core without it the CPU package, so CUDA alone decides between the GPU and the
# standard build. The GPU packages carry no headers, which come from the CPU package. The files
# are laid out as the GitHub packages were, include and lib, and ONNXRUNTIME_ROOT names another SDK.
set(SCOPEONE_ONNXRUNTIME_VERSION 1.30.0)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime SDK root; empty selects the official package in external")
set(_ort_cpu_package microsoft.ml.onnxruntime)
set(_ort_cpu_sha256 30df119bd9164eaa187666d93147448fdd4a9a1a3caf83f1505daf7af2d1c5d2)
set(_ort_package "")
if (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(_ort_rid win-x64)
    set(_ort_name "onnxruntime-win-x64")
    if (_with_cuda)
        set(_ort_package microsoft.ml.onnxruntime.gpu.windows)
        set(_ort_sha256 f86f88d2dcdbc1411a5a4f345751c37a85f965300e150da6d882bea41ea7296d)
        set(_ort_name "onnxruntime-win-x64-gpu_cuda12")
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(_ort_rid linux-x64)
    set(_ort_name "onnxruntime-linux-x64")
    if (_with_cuda)
        set(_ort_package microsoft.ml.onnxruntime.gpu.linux)
        set(_ort_sha256 77ca23682fc164789e67cd0fa4ce022b1b1c94e1978046c6446eee5fb0a3a3d6)
        set(_ort_name "onnxruntime-linux-x64-gpu_cuda12")
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_ort_rid linux-arm64)
    set(_ort_name "onnxruntime-linux-aarch64")
elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    # The macOS package has only the CPU provider
    set(_ort_rid osx-arm64)
    set(_ort_name "onnxruntime-osx-arm64")
endif ()
if (_ort_rid AND NOT _ort_package)
    set(_ort_package ${_ort_cpu_package})
    set(_ort_sha256 ${_ort_cpu_sha256})
endif ()

# Downloads one NuGet package, checks its hash and extracts it into destination. EXPECTED_HASH
# turned a failed download into a configure error, so the hash is checked after it, and without
# the network Core is built without ONNX inference and the next configure tries again.
function(scopeone_fetch_nupkg package sha256 destination error_variable)
    set(_url "https://api.nuget.org/v3-flatcontainer/${package}/${SCOPEONE_ONNXRUNTIME_VERSION}/${package}.${SCOPEONE_ONNXRUNTIME_VERSION}.nupkg")
    set(_archive "${CMAKE_CURRENT_BINARY_DIR}/${package}.${SCOPEONE_ONNXRUNTIME_VERSION}.zip")
    message(STATUS "Downloading ${_url}")
    file(DOWNLOAD "${_url}" "${_archive}" SHOW_PROGRESS STATUS _status)
    list(GET _status 0 _code)
    list(GET _status 1 _message)
    if (_code EQUAL 0)
        file(SHA256 "${_archive}" _actual)
        if (_actual STREQUAL sha256)
            set(_message "")
            file(REMOVE_RECURSE "${destination}")
            file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${destination}")
        else ()
            set(_message "SHA256 mismatch")
        endif ()
    endif ()
    file(REMOVE "${_archive}")
    if (_message)
        set(${error_variable} "downloading ${_url} failed (${_message})" PARENT_SCOPE)
    else ()
        set(${error_variable} "" PARENT_SCOPE)
    endif ()
endfunction()

set(_ort_download_error "")
if (ONNXRUNTIME_ROOT)
    set(_ort_root "${ONNXRUNTIME_ROOT}")
elseif (_ort_package)
    set(_ort_root "${CMAKE_CURRENT_SOURCE_DIR}/external/${_ort_name}-${SCOPEONE_ONNXRUNTIME_VERSION}")
    if (NOT EXISTS "${_ort_root}/include")
        set(_ort_cpu_dir "${CMAKE_CURRENT_BINARY_DIR}/${_ort_cpu_package}")
        set(_ort_dir "${CMAKE_CURRENT_BINARY_DIR}/${_ort_package}")
        scopeone_fetch_nupkg(${_ort_cpu_package} ${_ort_cpu_sha256} "${_ort_cpu_dir}" _ort_download_error)
        if (NOT _ort_download_error AND NOT _ort_package STREQUAL _ort_cpu_package)
            scopeone_fetch_nupkg(${_ort_package} ${_ort_sha256} "${_ort_dir}" _ort_download_error)
        endif ()
        if (NOT _ort_download_error)
            file(COPY "${_ort_cpu_dir}/build/native/include/" DESTINATION "${_ort_root}/include")
            file(GLOB _ort_native "${_ort_dir}/runtimes/${_ort_rid}/native/*")
            file(COPY ${_ort_native} DESTINATION "${_ort_root}/lib")
            # The library is loaded by its soname, libonnxruntime.so.1 or libonnxruntime.1.dylib,
            # which the package does not have as a file, and linked by the plain name
            if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
                file(RENAME "${_ort_root}/lib/libonnxruntime.so" "${_ort_root}/lib/libonnxruntime.so.1")
                file(CREATE_LINK libonnxruntime.so.1 "${_ort_root}/lib/libonnxruntime.so" SYMBOLIC)
            elseif (APPLE)
                file(RENAME "${_ort_root}/lib/libonnxruntime.dylib" "${_ort_root}/lib/libonnxruntime.1.dylib")
                file(CREATE_LINK libonnxruntime.1.dylib "${_ort_root}/lib/libonnxruntime.dylib" SYMBOLIC)
            endif ()
        endif ()
        file(REMOVE_RECURSE "${_ort_cpu_dir}" "${_ort_dir}")
    endif ()
else ()
    set(_ort_root "")
endif ()

# The found paths are cached, so a root that changed, such as the GPU package taking over
# once CUDA is found, kept linking the package of the earlier configure
if (NOT "${SCOPEONE_ONNXRUNTIME_ROOT_USED}" STREQUAL "${_ort_root}")
    unset(ONNXRUNTIME_INCLUDE_DIR CACHE)
    unset(ONNXRUNTIME_LIBRARY CACHE)
endif ()
set(SCOPEONE_ONNXRUNTIME_ROOT_USED "${_ort_root}" CACHE INTERNAL "")
set(ONNXRUNTIME_ROOT "${_ort_root}")
find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATHS "${ONNXRUNTIME_ROOT}/include"
        NO_DEFAULT_PATH
)
find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        PATHS "${ONNXRUNTIME_ROOT}/lib"
        NO_DEFAULT_PATH
)
if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    file(GLOB _inference_sources CONFIGURE_DEPENDS src/inference/*.cpp internal/inference/*.h)
    target_sources(ScopeOneCore PRIVATE ${_inference_sources})
    target_include_directories(ScopeOneCore PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(ScopeOneCore PRIVATE "${ONNXRUNTIME_LIBRARY}")
    set(_with_inference ON)
    if (WIN32)
        file(GLOB _onnxruntime_dlls
                "${ONNXRUNTIME_ROOT}/bin/onnxruntime*.dll"
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime*.dll"
        )
        if (NOT _onnxruntime_dlls)
            message(FATAL_ERROR "No ONNX Runtime DLLs found under ${ONNXRUNTIME_ROOT}")
        endif ()
        list(APPEND SCOPEONE_RUNTIME_DLLS ${_onnxruntime_dlls})
    else ()
        # The installed Core resolves its libraries beside itself. Left in external, ONNX Runtime
        # was not on its runpath and the GUI failed to link against the installed Core.
        file(GLOB _onnxruntime_libs
                "${ONNXRUNTIME_ROOT}/lib/libonnxruntime*.so*"
                "${ONNXRUNTIME_ROOT}/lib/libonnxruntime*.dylib"
        )
        install(FILES ${_onnxruntime_libs} DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif ()
elseif (NOT ONNXRUNTIME_ROOT)
    set(_inference_reason "no official ONNX Runtime package for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; set ONNXRUNTIME_ROOT")
    message(STATUS "${_inference_reason}; building Core without ONNX inference")
elseif (_ort_download_error)
    set(_inference_reason "${_ort_download_error}")
    message(STATUS "${_inference_reason}; building Core without ONNX inference")
else ()
    set(_inference_reason "ONNX Runtime was not found under ${ONNXRUNTIME_ROOT}")
    message(STATUS "${_inference_reason}; building Core without ONNX inference")
endif ()
# What this configure built in, for the build scripts to report
set(SCOPEONE_BUILT_WITH_CUDA "${_with_cuda}" CACHE INTERNAL "")
set(SCOPEONE_BUILT_WITH_INFERENCE "${_with_inference}" CACHE INTERNAL "")
set(SCOPEONE_CUDA_SKIP_REASON "${_cuda_reason}" CACHE INTERNAL "")
set(SCOPEONE_INFERENCE_SKIP_REASON "${_inference_reason}" CACHE INTERNAL "")
target_include_directories(ScopeOneCore
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MMCORE_INCLUDE_DIR}
        ${MMDEVICE_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
        ${POCKETFFT_INCLUDE_DIR}
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
# Keeps the symbols of the static libraries in Core, MMCore among them, out of its exports. Linux
# exported them all, so DriverHost had linked MMCore through Core without noticing.
if (UNIX AND NOT APPLE)
    target_link_options(ScopeOneCore PRIVATE "LINKER:--exclude-libs,ALL")
endif ()

scopeone_add_runtime_copy_commands(ScopeOneCore $<TARGET_FILE_DIR:ScopeOneCore>)
add_custom_command(TARGET ScopeOneCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ScopeWriter>
        $<TARGET_FILE_DIR:ScopeOneCore>
)

# DriverHost does not link Core, so a driver process loads only the SDK, Qt Core and Network and
# MMCore. It builds the few Core sources it needs itself.
add_executable(ScopeOne_DriverHost
        src/DriverHostMain.cpp
        src/HardwareStreamCodec.cpp
        src/DriverManifest.cpp
        src/MicroManagerCamera.cpp
        src/StreamRing.cpp
)
if (UNIX)
    set_target_properties(ScopeOne_DriverHost PROPERTIES
            BUILD_RPATH "${SCOPEONE_RPATH_ORIGIN}"
            INSTALL_RPATH "${SCOPEONE_RPATH_ORIGIN}/../lib;${SCOPEONE_RPATH_ORIGIN}"
            INSTALL_RPATH_USE_LINK_PATH TRUE
    )
endif ()
target_link_libraries(ScopeOne_DriverHost
        ScopeOneDriverSDK
        MMCore
        Qt::Core
        Qt::Network
)
target_compile_definitions(ScopeOne_DriverHost PRIVATE
        MMDEVICE_CLIENT_BUILD
        SCOPEONE_CORE_STATIC
        SCOPEONE_CORE_VERSION_STRING="${PROJECT_VERSION}"
)
target_include_directories(ScopeOne_DriverHost PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MMCORE_INCLUDE_DIR}
        ${MMDEVICE_INCLUDE_DIR}
)
target_link_directories(ScopeOne_DriverHost PRIVATE ${MMCORE_BIN_DIR})

scopeone_add_runtime_copy_commands(ScopeOne_DriverHost $<TARGET_FILE_DIR:ScopeOne_DriverHost>)

# /MP compiles the sources of one project in parallel, which the Visual Studio generator does not,
# and Release builds use LTO. Only for C++, nvcc takes neither.
if (MSVC)
    set(_cxx "$<COMPILE_LANGUAGE:CXX>")
    set(_release_cxx "$<AND:$<CONFIG:Release>,${_cxx}>")
    foreach (_target IN ITEMS ScopeOneCore ScopeOne_DriverHost)
        target_compile_options(${_target} PRIVATE
                "$<${_cxx}:/MP>"
                "$<${_release_cxx}:/GL>" "$<${_release_cxx}:/fp:fast>" "$<${_release_cxx}:/arch:AVX2>"
        )
        target_link_options(${_target} PRIVATE $<$<CONFIG:Release>:/LTCG>)
    endforeach ()
    target_compile_options(ScopeOneDriverSDK PRIVATE /MP)
endif ()


configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScopeOneCoreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCore"
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCoreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
)

install(TARGETS ScopeOneCore ScopeOneDriverSDK
        EXPORT ScopeOneCoreTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS ScopeOne_DriverHost
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING
        PATTERN "*.h"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sdk"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScopeOneDriver.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCore"
)

if (SCOPEONE_RUNTIME_DLLS)
    install(FILES ${SCOPEONE_RUNTIME_DLLS}
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif ()
