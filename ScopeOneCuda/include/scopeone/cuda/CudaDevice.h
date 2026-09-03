#pragma once

#include "scopeone/cuda/CudaExport.h"

#include <QString>

namespace scopeone::cuda
{
    SCOPEONE_CUDA_EXPORT bool isCudaDeviceAvailable();
    SCOPEONE_CUDA_EXPORT int cudaDeviceCount();
    SCOPEONE_CUDA_EXPORT QString cudaDeviceName(int deviceIndex = 0);
}
