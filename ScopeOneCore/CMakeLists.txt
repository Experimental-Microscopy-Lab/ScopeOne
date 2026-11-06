cmake_minimum_required(VERSION 3.23)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ScopeOneVersion.cmake")
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

find_package(Qt6 COMPONENTS Core Gui Network Concurrent REQUIRED)
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)

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

set(CORE_SOURCES
        src/ExperimentDocument.cpp
        src/AcquisitionEngine.cpp
        src/DriverHostProviderProxy.cpp
        src/DaqDeviceManager.cpp
        src/DriverManifest.cpp
        src/HardwareRuntime.cpp
        src/MicroManagerProvider.cpp
        src/ImageOps.cpp
        src/ImageSceneModel.cpp
        src/MMCoreManager.cpp
        src/FrameBufferUtils.cpp
        src/ParticleAnalysis.cpp
        src/StageMosaicManager.cpp
        src/ImageProcessingFramework.cpp
        src/ProcessingPipeline.cpp
        src/DhmReconstruction.cpp
        src/ProcessingModuleRegistry.cpp
        src/SpatiotemporalBinningModule.cpp
        src/GaussianBlurModule.cpp
        src/ImageOperationModules.cpp
        src/FFTModule.cpp
        src/FrequencyDomainFilterModule.cpp
        src/MaskModule.cpp
        src/IFFTModule.cpp
        src/BackgroundCalibrationModule.cpp
        src/DifferentialRollingModule.cpp
        src/IscatModule.cpp
        src/PythonProcessingModule.cpp
        src/PtuFile.cpp
        src/HardwareStreamCodec.cpp
        src/StreamRing.cpp
        src/MDAManager.cpp
        src/RecordingManager.cpp
        src/CameraBackend.cpp
        src/CameraManager.cpp
        src/NativeCameraBackend.cpp
        src/DriverHostCameraBackend.cpp
        src/ScopeOneCore.cpp
        src/ScanImageAssembler.cpp
        src/SignalSourceManager.cpp
)

set(CORE_HEADERS
        include/scopeone/ExperimentDocument.h
        include/scopeone/DriverManifest.h
        include/scopeone/ProcessingTypes.h
        internal/ScanImageAssembler.h
        include/scopeone/SharedFrame.h
        include/scopeone/DhmReconstruction.h
        internal/AcquisitionEngine.h
        internal/CameraRuntimeControl.h
        include/scopeone/ImageOps.h
        include/scopeone/ImageSceneModel.h
        internal/HardwareRuntime.h
        internal/MicroManagerProvider.h
        internal/MMCoreManager.h
        internal/FrameBufferUtils.h
        internal/ParticleAnalysis.h
        internal/StageMosaicManager.h
        internal/ImageProcessingFramework.h
        internal/ProcessingModule.h
        internal/ProcessingModuleRegistry.h
        include/scopeone/ProcessingPipeline.h
        internal/DriverHostProtocol.h
        internal/SharedFrameRing.h
        internal/HardwareStreamCodec.h
        internal/StreamRing.h
        internal/DriverHostProviderProxy.h
        internal/DaqDeviceManager.h
        internal/SignalSourceManager.h
        internal/SpatiotemporalBinningModule.h
        internal/GaussianBlurModule.h
        internal/ImageOperationModules.h
        internal/FFTModule.h
        internal/FrequencyDomainFilterModule.h
        internal/MaskModule.h
        internal/IFFTModule.h
        internal/BackgroundCalibrationModule.h
        internal/DifferentialRollingModule.h
        internal/IscatModule.h
        internal/PythonProcessingModule.h
        internal/PtuFile.h
        internal/MDAManager.h
        internal/RecordingManager.h
        internal/CameraBackend.h
        internal/CameraManager.h
        include/scopeone/scopeone_core_export.h
        include/scopeone/RecordingSession.h
        include/scopeone/ScopeOneCore.h
)

# The driver SDK: the only headers and the only library a driver sees. It is a library of its
# own so a driver that reaches for anything else of Core does not compile or link.
set(SDK_HEADERS
        sdk/include/scopeone/CameraProvider.h
        sdk/include/scopeone/DaqDevice.h
        sdk/include/scopeone/DaqProvider.h
        sdk/include/scopeone/Driver.h
        sdk/include/scopeone/HardwareCapabilities.h
        sdk/include/scopeone/DriverSDK.h
        sdk/include/scopeone/HardwareProvider.h
        sdk/include/scopeone/HardwareTypes.h
        sdk/include/scopeone/ImageFrame.h
        sdk/include/scopeone/scopeone_sdk_export.h
        sdk/include/scopeone/SignalSource.h
        sdk/include/scopeone/SignalSourceProvider.h
)
add_library(ScopeOneDriverSDK SHARED
        sdk/src/HardwareProvider.cpp
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

# The CUDA and ONNX Runtime processing modules are part of Core when their toolkits are found.
# Turning them off builds the plain CPU Core, which neither links nor ships CUDA or ONNX Runtime.
set(_with_cuda OFF)
set(_with_inference OFF)
# Why a module that is turned on was left out, for the summary of the build scripts
set(_cuda_reason "")
set(_inference_reason "")
option(SCOPEONE_ENABLE_CUDA "Build the CUDA processing modules into Core when nvcc is available" ON)
option(SCOPEONE_ENABLE_INFERENCE "Build the ONNX Runtime processing module into Core when ONNX Runtime is available" ON)
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
    target_sources(ScopeOneCore PRIVATE
            src/cuda/CudaKernels.cu
            src/cuda/CudaDevice.cpp
            src/cuda/CudaFrequencyFilterModule.cpp
            src/cuda/CudaGaussianBlurModule.cpp
            src/cuda/CudaProcessingRegistration.cpp
            src/cuda/CudaRealImageModule.cpp
            src/cuda/GpuComplexFrame.cpp
            src/cuda/GpuRealFrame.cpp
            internal/cuda/CudaDevice.h
            internal/cuda/CudaFrequencyFilterModule.h
            internal/cuda/CudaGaussianBlurModule.h
            internal/cuda/CudaKernelLaunch.h
            internal/cuda/CudaProcessingRegistration.h
            internal/cuda/CudaRealImageModule.h
            internal/cuda/GpuComplexFrame.h
            internal/cuda/GpuRealFrame.h
    )
    set_source_files_properties(src/cuda/CudaKernels.cu PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_target_properties(ScopeOneCore PROPERTIES
            CUDA_ARCHITECTURES "${SCOPEONE_CUDA_ARCHITECTURES}"
            CUDA_SEPARABLE_COMPILATION OFF
    )
    target_link_libraries(ScopeOneCore PRIVATE CUDA::cudart CUDA::cufft)
    target_compile_definitions(ScopeOneCore PRIVATE SCOPEONE_WITH_CUDA)
    set(_with_cuda ON)
    if (WIN32)
        file(GLOB _cuda_runtime_dlls
                "${CUDAToolkit_BIN_DIR}/cudart64_*.dll"
                "${CUDAToolkit_BIN_DIR}/cufft64_*.dll"
        )
        list(APPEND SCOPEONE_RUNTIME_DLLS ${_cuda_runtime_dlls})
    endif ()
endif ()

if (SCOPEONE_ENABLE_INFERENCE)
    set(ONNXRUNTIME_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/external/onnxruntime-win-x64-gpu_cuda12-1.30.0"
            CACHE PATH "ONNX Runtime SDK root"
    )
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
endif ()
if (SCOPEONE_ENABLE_INFERENCE AND ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    target_sources(ScopeOneCore PRIVATE
            src/inference/InferenceProcessingRegistration.cpp
            src/inference/OnnxInferenceModule.cpp
            src/inference/OnnxRuntime.cpp
            src/inference/OnnxSession.cpp
            internal/inference/ExecutionProvider.h
            internal/inference/InferenceProcessingRegistration.h
            internal/inference/OnnxInferenceModule.h
            internal/inference/OnnxInternal.h
            internal/inference/OnnxRuntime.h
            internal/inference/OnnxSession.h
            internal/inference/TensorInfo.h
    )
    target_include_directories(ScopeOneCore PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(ScopeOneCore PRIVATE "${ONNXRUNTIME_LIBRARY}")
    target_compile_definitions(ScopeOneCore PRIVATE SCOPEONE_WITH_INFERENCE)
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
    endif ()
elseif (SCOPEONE_ENABLE_INFERENCE)
    set(_inference_reason "ONNX Runtime was not found under ${ONNXRUNTIME_ROOT}")
    message(STATUS "${_inference_reason}; building Core without ONNX inference")
else ()
    set(_inference_reason "turned off with SCOPEONE_ENABLE_INFERENCE")
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

# The Visual Studio generator compiles the sources of one project one after another unless the
# compiler runs them in parallel itself. Only for C++, nvcc does not take /MP.
if (MSVC)
    target_compile_options(ScopeOneCore PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/MP>)
    target_compile_options(ScopeOne_DriverHost PRIVATE /MP)
    target_compile_options(ScopeOneDriverSDK PRIVATE /MP)
endif ()

# LTO Release build
if (MSVC)
    # Only for C++, nvcc does not take these flags
    set(_release_cxx "$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>")
    target_compile_options(ScopeOneCore PRIVATE
            "$<${_release_cxx}:/GL>" "$<${_release_cxx}:/fp:fast>" "$<${_release_cxx}:/arch:AVX2>"
    )
    target_link_options(ScopeOneCore PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )

    target_compile_options(ScopeOne_DriverHost PRIVATE
            $<$<CONFIG:Release>:/GL /fp:fast /arch:AVX2>
    )
    target_link_options(ScopeOne_DriverHost PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )
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
