#include "internal/cuda/CudaFrequencyFilterKernels.h"
#include "internal/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
    using scopeone::core::internal::cuda::detail::synchronizeKernel;

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

namespace scopeone::core::internal::cuda::detail
{
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
