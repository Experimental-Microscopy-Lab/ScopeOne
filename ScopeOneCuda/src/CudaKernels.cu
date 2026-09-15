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

    __device__ int reflect101(int value, int size)
    {
        while (value < 0 || value >= size)
        {
            value = value < 0 ? -value : 2 * size - value - 2;
        }
        return value;
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
            const int sampleY = reflect101(y + dy, height);
            const float* inputRow = reinterpret_cast<const float*>(
                reinterpret_cast<const unsigned char*>(input)
                + static_cast<std::size_t>(sampleY) * inputPitchBytes);
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const int sampleX = reflect101(x + dx, width);
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
                                        float minFeatureSize,
                                        float maxFeatureSize,
                                        int filterKind)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= spectrumWidth || y >= height)
        {
            return;
        }

        constexpr float twoPi = 6.28318530717958647692f;
        const float fx = twoPi * static_cast<float>(x) / static_cast<float>(realWidth);
        const float fy = static_cast<float>(y <= height / 2 ? y : y - height)
            * twoPi / static_cast<float>(height);
        const float radiusSquared = fx * fx + fy * fy;
        const float mask = filterKind == 1
                               ? (radiusSquared * maxFeatureSize * maxFeatureSize > 1.0f
                                  && radiusSquared * minFeatureSize * minFeatureSize < 1.0f
                                      ? 1.0f
                                      : 0.0f)
                               : expf(-radiusSquared * minFeatureSize * minFeatureSize * 0.5f)
                                   - expf(-radiusSquared * maxFeatureSize * maxFeatureSize * 0.5f);
        spectrum[y * spectrumWidth + x].x *= mask;
        spectrum[y * spectrumWidth + x].y *= mask;
    }

    __global__ void renderSpectrumKernel(const float2* spectrum,
                                         int spectrumWidth,
                                         int width,
                                         int height,
                                         float* output)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }
        const int sourceX = (x + width / 2) % width;
        const int sourceY = (y + height / 2) % height;
        float2 value;
        if (sourceX <= width / 2)
        {
            value = spectrum[sourceY * spectrumWidth + sourceX];
        }
        else
        {
            const float2 conjugate = spectrum[((height - sourceY) % height) * spectrumWidth
                                               + width - sourceX];
            value = make_float2(conjugate.x, -conjugate.y);
        }
        output[y * width + x] = log1pf(hypotf(value.x, value.y));
    }

    __global__ void scaleKernel(float* output, int count, float scale)
    {
        const int index = blockIdx.x * blockDim.x + threadIdx.x;
        if (index < count)
        {
            output[index] *= scale;
        }
    }

    __device__ unsigned int orderedFloat(float value)
    {
        const unsigned int bits = __float_as_uint(value);
        return (bits & 0x80000000U) == 0 ? bits | 0x80000000U : ~bits;
    }

    __device__ float floatFromOrdered(unsigned int value)
    {
        const unsigned int bits = (value & 0x80000000U) == 0 ? ~value : value & ~0x80000000U;
        return __uint_as_float(bits);
    }

    __global__ void reduceMinMaxKernel(const float* input, int count, unsigned int* limits)
    {
        __shared__ unsigned int blockMinimum;
        __shared__ unsigned int blockMaximum;
        if (threadIdx.x == 0)
        {
            blockMinimum = 0xFF800000U;
            blockMaximum = 0x007FFFFFU;
        }
        __syncthreads();
        const int index = blockIdx.x * blockDim.x + threadIdx.x;
        if (index < count)
        {
            const unsigned int value = orderedFloat(input[index]);
            atomicMin(&blockMinimum, value);
            atomicMax(&blockMaximum, value);
        }
        __syncthreads();
        if (threadIdx.x == 0)
        {
            atomicMin(&limits[0], blockMinimum);
            atomicMax(&limits[1], blockMaximum);
        }
    }

    __global__ void minMaxNormalizeKernel(float* output,
                                          int count,
                                          const unsigned int* limits,
                                          float maxValue)
    {
        const int index = blockIdx.x * blockDim.x + threadIdx.x;
        if (index < count)
        {
            const float minimum = floatFromOrdered(limits[0]);
            const float maximum = floatFromOrdered(limits[1]);
            output[index] = maximum == minimum
                                ? 0.0f
                                : (output[index] - minimum) * maxValue / (maximum - minimum);
        }
    }

    bool synchronizeKernel()
    {
        return cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
    }

    bool normalizeMinMax(float* output, int count, void* scratch, float maxValue)
    {
        const unsigned int limits[] = {0xFF800000U, 0x007FFFFFU};
        if (cudaMemcpy(scratch, limits, sizeof(limits), cudaMemcpyHostToDevice) != cudaSuccess)
        {
            return false;
        }
        constexpr int blockSize = 256;
        reduceMinMaxKernel<<<(count + blockSize - 1) / blockSize, blockSize>>>(
            output,
            count,
            static_cast<unsigned int*>(scratch));
        minMaxNormalizeKernel<<<(count + blockSize - 1) / blockSize, blockSize>>>(
            output,
            count,
            static_cast<const unsigned int*>(scratch),
            maxValue);
        return synchronizeKernel();
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
                        int kernelSize,
                        float sigma)
    {
        kernelSize = std::max(1, kernelSize);
        if ((kernelSize % 2) == 0)
        {
            ++kernelSize;
        }
        const float effectiveSigma = sigma > 0.0f
                                         ? sigma
                                         : 0.3f * ((kernelSize - 1) * 0.5f - 1.0f) + 0.8f;
        const int radius = kernelSize / 2;
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
            effectiveSigma);
        return synchronizeKernel();
    }

    bool launchFrequencyFilter(const void* input,
                               void* output,
                               void* spectrum,
                               int width,
                               int height,
                               float minFeatureSize,
                               float maxFeatureSize,
                               int filterKind,
                               int outputMode,
                               int forwardPlan,
                               int inversePlan,
                               void* minMaxScratch,
                               float maxValue)
    {
        if (cufftExecR2C(forwardPlan,
                         static_cast<cufftReal*>(const_cast<void*>(input)),
                         static_cast<cufftComplex*>(spectrum)) != CUFFT_SUCCESS)
        {
            return false;
        }

        const dim3 block(16, 16);
        const dim3 imageGrid((width + block.x - 1) / block.x,
                             (height + block.y - 1) / block.y);
        const int spectrumWidth = width / 2 + 1;
        const dim3 grid((spectrumWidth + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        if (outputMode == 0)
        {
            renderSpectrumKernel<<<imageGrid, block>>>(static_cast<const float2*>(spectrum),
                                                        spectrumWidth,
                                                        width,
                                                        height,
                                                        static_cast<float*>(output));
            return synchronizeKernel()
                && normalizeMinMax(static_cast<float*>(output),
                                   width * height,
                                   minMaxScratch,
                                   maxValue);
        }
        frequencyMaskKernel<<<grid, block>>>(
            static_cast<cufftComplex*>(spectrum),
            spectrumWidth,
            height,
            width,
            minFeatureSize,
            maxFeatureSize,
            filterKind);
        if (!synchronizeKernel())
        {
            return false;
        }

        if (outputMode == 1)
        {
            renderSpectrumKernel<<<imageGrid, block>>>(static_cast<const float2*>(spectrum),
                                                        spectrumWidth,
                                                        width,
                                                        height,
                                                        static_cast<float*>(output));
            return synchronizeKernel()
                && normalizeMinMax(static_cast<float*>(output),
                                   width * height,
                                   minMaxScratch,
                                   maxValue);
        }

        if (cufftExecC2R(inversePlan,
                         static_cast<cufftComplex*>(spectrum),
                         static_cast<cufftReal*>(output)) != CUFFT_SUCCESS)
        {
            return false;
        }
        constexpr int blockSize = 256;
        scaleKernel<<<(width * height + blockSize - 1) / blockSize, blockSize>>>(
            static_cast<float*>(output),
            width * height,
            1.0f / static_cast<float>(width * height));
        return synchronizeKernel()
            && normalizeMinMax(static_cast<float*>(output),
                               width * height,
                               minMaxScratch,
                               maxValue);
    }
}
