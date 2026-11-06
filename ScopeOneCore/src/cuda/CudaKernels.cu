#include "internal/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>

#include <cstdint>

namespace
{
    __global__ void convertToFloatKernel(const unsigned char* source,
                                          int sourceStride,
                                          int width,
                                          int height,
                                          int sourceBytesPerPixel,
                                          float* destination,
                                          std::size_t destinationPitchBytes)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }

        const unsigned char* sourceRow = source + static_cast<std::size_t>(y) * sourceStride;
        float* destinationRow = reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(destination)
            + static_cast<std::size_t>(y) * destinationPitchBytes);
        if (sourceBytesPerPixel == 1)
        {
            destinationRow[x] = static_cast<float>(sourceRow[x]);
        }
        else
        {
            destinationRow[x] = static_cast<float>(
                reinterpret_cast<const std::uint16_t*>(sourceRow)[x]);
        }
    }

    __global__ void convertFromFloatKernel(const float* source,
                                            std::size_t sourcePitchBytes,
                                            unsigned char* destination,
                                            int destinationStride,
                                            int width,
                                            int height,
                                            int destinationBytesPerPixel,
                                            float scale,
                                            float offset)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }

        const float* sourceRow = reinterpret_cast<const float*>(
            reinterpret_cast<const unsigned char*>(source)
            + static_cast<std::size_t>(y) * sourcePitchBytes);
        unsigned char* destinationRow = destination
            + static_cast<std::size_t>(y) * destinationStride;
        const float value = fminf(fmaxf(sourceRow[x] * scale + offset, 0.0f), 65535.0f);
        if (destinationBytesPerPixel == 1)
        {
            destinationRow[x] = static_cast<unsigned char>(fminf(value, 255.0f) + 0.5f);
        }
        else
        {
            reinterpret_cast<std::uint16_t*>(destinationRow)[x] =
                static_cast<std::uint16_t>(value + 0.5f);
        }
    }
}

namespace scopeone::core::internal::cuda::detail
{
    bool synchronizeKernel()
    {
        return cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
    }

    bool convertToFloat(const void* source,
                        int sourceStride,
                        int width,
                        int height,
                        int sourceBytesPerPixel,
                        void* destination,
                        std::size_t destinationPitchBytes)
    {
        const dim3 block(16, 16);
        const dim3 grid((width + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        convertToFloatKernel<<<grid, block>>>(
            static_cast<const unsigned char*>(source),
            sourceStride,
            width,
            height,
            sourceBytesPerPixel,
            static_cast<float*>(destination),
            destinationPitchBytes);
        return synchronizeKernel();
    }

    bool convertFromFloat(const void* source,
                          std::size_t sourcePitchBytes,
                          void* destination,
                          int destinationStride,
                          int width,
                          int height,
                          int destinationBytesPerPixel,
                          float scale,
                          float offset)
    {
        const dim3 block(16, 16);
        const dim3 grid((width + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        convertFromFloatKernel<<<grid, block>>>(
            static_cast<const float*>(source),
            sourcePitchBytes,
            static_cast<unsigned char*>(destination),
            destinationStride,
            width,
            height,
            destinationBytesPerPixel,
            scale,
            offset);
        return synchronizeKernel();
    }
}
