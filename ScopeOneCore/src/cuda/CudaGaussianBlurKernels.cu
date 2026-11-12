#include "internal/cuda/CudaGaussianBlurKernels.h"
#include "internal/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>

namespace
{
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
}

namespace scopeone::core::internal::cuda::detail
{
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
}
