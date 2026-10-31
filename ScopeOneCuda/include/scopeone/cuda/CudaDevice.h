#pragma once

#include "scopeone/cuda/CudaExport.h"

namespace scopeone::cuda
{
    SCOPEONE_CUDA_EXPORT bool isCudaDeviceAvailable();
    SCOPEONE_CUDA_EXPORT int cudaDeviceCount();
}
