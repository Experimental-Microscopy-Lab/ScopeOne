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

    QString cudaDeviceName(int deviceIndex)
    {
        cudaDeviceProp properties{};
        return cudaGetDeviceProperties(&properties, deviceIndex) == cudaSuccess
                   ? QString::fromUtf8(properties.name)
                   : QString();
    }
}
