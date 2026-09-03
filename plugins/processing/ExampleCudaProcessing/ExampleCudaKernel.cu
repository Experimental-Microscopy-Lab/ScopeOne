#include <cuda_runtime.h>

#include <cstddef>

namespace
{
    __global__ void invertKernel(const float* input,
                                 std::size_t inputPitchBytes,
                                 float* output,
                                 std::size_t outputPitchBytes,
                                 int width,
                                 int height)
    {
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        if (x >= width || y >= height)
        {
            return;
        }

        const float* inputRow = reinterpret_cast<const float*>(
            reinterpret_cast<const unsigned char*>(input)
            + static_cast<std::size_t>(y) * inputPitchBytes);
        float* outputRow = reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(output)
            + static_cast<std::size_t>(y) * outputPitchBytes);
        outputRow[x] = 65535.0f - inputRow[x];
    }
}

namespace example_cuda
{
    bool launchInvert(const void* input,
                      std::size_t inputPitchBytes,
                      void* output,
                      std::size_t outputPitchBytes,
                      int width,
                      int height)
    {
        const dim3 block(16, 16);
        const dim3 grid((width + block.x - 1) / block.x,
                        (height + block.y - 1) / block.y);
        invertKernel<<<grid, block>>>(
            static_cast<const float*>(input),
            inputPitchBytes,
            static_cast<float*>(output),
            outputPitchBytes,
            width,
            height);
        return cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
    }
}
