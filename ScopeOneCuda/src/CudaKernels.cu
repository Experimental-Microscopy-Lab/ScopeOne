#include "scopeone/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
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
                                            int destinationBytesPerPixel)
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
        const float value = fminf(fmaxf(sourceRow[x], 0.0f), 65535.0f);
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

    __global__ void gaussianKernel(const float* input,
                                   std::size_t inputPitchBytes,
                                   float* output,
                                   std::size_t outputPitchBytes,
                                   int width,
                                   int height,
                                   int radius,
                                   float sigma)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }

        float sum = 0.0f;
        float weightSum = 0.0f;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            const int sampleY = min(max(y + dy, 0), height - 1);
            const float* inputRow = reinterpret_cast<const float*>(
                reinterpret_cast<const unsigned char*>(input)
                + static_cast<std::size_t>(sampleY) * inputPitchBytes);
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const int sampleX = min(max(x + dx, 0), width - 1);
                const float distanceSquared = static_cast<float>(dx * dx + dy * dy);
                const float weight = expf(-distanceSquared / (2.0f * sigma * sigma));
                sum += inputRow[sampleX] * weight;
                weightSum += weight;
            }
        }

        float* outputRow = reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(output)
            + static_cast<std::size_t>(y) * outputPitchBytes);
        outputRow[x] = sum / weightSum;
    }

    __global__ void frequencyMaskKernel(float2* spectrum,
                                        int spectrumWidth,
                                        int height,
                                        int realWidth,
                                        float lowCutoff,
                                        float highCutoff)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= spectrumWidth || y >= height)
        {
            return;
        }

        const float fx = static_cast<float>(x) / static_cast<float>(realWidth);
        const float fy = static_cast<float>(y <= height / 2 ? y : y - height)
            / static_cast<float>(height);
        const float radius = sqrtf(fx * fx + fy * fy);
        const bool keep = radius >= lowCutoff && radius <= highCutoff;
        if (!keep)
        {
            spectrum[y * spectrumWidth + x] = make_float2(0.0f, 0.0f);
        }
    }

    __global__ void normalizeKernel(float* output, int width, int height, float scale)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x < width && y < height)
        {
            output[y * width + x] *= scale;
        }
    }

    bool synchronizeKernel()
    {
        return cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
    }
}

namespace scopeone::cuda::detail
{
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
                          int destinationBytesPerPixel)
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
            destinationBytesPerPixel);
        return synchronizeKernel();
    }

    bool launchGaussian(const void* input,
                        std::size_t inputPitchBytes,
                        void* output,
                        std::size_t outputPitchBytes,
                        int width,
                        int height,
                        float sigma)
    {
        const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
        const dim3 block(16, 16);
        const dim3 grid((width + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        gaussianKernel<<<grid, block>>>(
            static_cast<const float*>(input),
            inputPitchBytes,
            static_cast<float*>(output),
            outputPitchBytes,
            width,
            height,
            radius,
            sigma);
        return synchronizeKernel();
    }

    bool launchFrequencyFilter(const void* input,
                               void* output,
                               void* spectrum,
                               int width,
                               int height,
                               float lowCutoff,
                               float highCutoff,
                               int forwardPlan,
                               int inversePlan)
    {
        if (cufftExecR2C(forwardPlan,
                         static_cast<cufftReal*>(const_cast<void*>(input)),
                         static_cast<cufftComplex*>(spectrum)) != CUFFT_SUCCESS)
        {
            return false;
        }

        const int spectrumWidth = width / 2 + 1;
        const dim3 block(16, 16);
        const dim3 grid((spectrumWidth + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        frequencyMaskKernel<<<grid, block>>>(
            static_cast<cufftComplex*>(spectrum),
            spectrumWidth,
            height,
            width,
            lowCutoff,
            highCutoff);
        if (!synchronizeKernel())
        {
            return false;
        }

        if (cufftExecC2R(inversePlan,
                         static_cast<cufftComplex*>(spectrum),
                         static_cast<cufftReal*>(output)) != CUFFT_SUCCESS)
        {
            return false;
        }
        const dim3 normalizeGrid((width + block.x - 1) / block.x,
                                 (height + block.y - 1) / block.y);
        normalizeKernel<<<normalizeGrid, block>>>(
            static_cast<float*>(output),
            width,
            height,
            1.0f / static_cast<float>(width * height));
        return synchronizeKernel();
    }
}
