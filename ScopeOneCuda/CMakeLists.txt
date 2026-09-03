cmake_minimum_required(VERSION 3.23)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(SCOPEONE_CUDA_VERSION 1.0.0)

add_library(ScopeOneCuda SHARED
        src/CudaKernels.cu
        src/CudaDevice.cpp
        src/CudaFrequencyFilterModule.cpp
        src/CudaFrequencyFilterModule.h
        src/CudaGaussianBlurModule.cpp
        src/CudaGaussianBlurModule.h
        src/CudaProcessingRegistration.cpp
        src/CudaRealImageModule.cpp
        src/GpuComplexFrame.cpp
        src/GpuRealFrame.cpp
        include/scopeone/cuda/CudaDevice.h
        include/scopeone/cuda/CudaRealImageModule.h
        include/scopeone/cuda/CudaKernelLaunch.h
        include/scopeone/cuda/GpuComplexFrame.h
        include/scopeone/cuda/CudaExport.h
        include/scopeone/cuda/GpuRealFrame.h
)
add_library(scopeone::Cuda ALIAS ScopeOneCuda)

set(SCOPEONE_CUDA_ARCHITECTURES "50;75;80;86;89" CACHE STRING
        "Target CUDA architectures")
set_target_properties(ScopeOneCuda PROPERTIES
        EXPORT_NAME Cuda
        CUDA_SEPARABLE_COMPILATION OFF
        CUDA_ARCHITECTURES "${SCOPEONE_CUDA_ARCHITECTURES}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_features(ScopeOneCuda PUBLIC cxx_std_20)
target_compile_definitions(ScopeOneCuda PRIVATE SCOPEONE_CUDA_EXPORTS)
target_include_directories(ScopeOneCuda
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(ScopeOneCuda
        PUBLIC
        scopeone::ScopeOneCore
        CUDA::cudart
        CUDA::cufft
)

file(GLOB SCOPEONE_CUDA_RUNTIME_DLLS
        "${CUDAToolkit_BIN_DIR}/cudart64_*.dll"
        "${CUDAToolkit_BIN_DIR}/cufft64_*.dll"
)
if (WIN32)
    foreach (_dll IN LISTS SCOPEONE_CUDA_RUNTIME_DLLS)
        add_custom_command(TARGET ScopeOneCuda POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}"
                "$<TARGET_FILE_DIR:ScopeOneCuda>"
        )
    endforeach ()
endif ()

install(TARGETS ScopeOneCuda
        EXPORT ScopeOneCudaTargets
        RUNTIME DESTINATION "."
        LIBRARY DESTINATION "."
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if (SCOPEONE_CUDA_RUNTIME_DLLS)
    install(FILES ${SCOPEONE_CUDA_RUNTIME_DLLS} DESTINATION ".")
endif ()
install(EXPORT ScopeOneCudaTargets
        FILE ScopeOneCudaTargets.cmake
        NAMESPACE scopeone::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCuda"
)

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScopeOneCudaConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCudaConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCuda"
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCudaConfigVersion.cmake"
        VERSION ${SCOPEONE_CUDA_VERSION}
        COMPATIBILITY SameMajorVersion
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCudaConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeOneCudaConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeOneCuda"
)
