#include "scopeone/cuda/CudaDevice.h"

#include <cuda_runtime.h>

namespace scopeone::cuda
{
    bool isCudaDeviceAvailable()
    {
        return cudaDeviceCount() > 0;
    }

    int cudaDeviceCount()
    {
        int count = 0;
        return cudaGetDeviceCount(&count) == cudaSuccess ? count : 0;
    }
}
