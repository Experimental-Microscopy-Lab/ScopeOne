#pragma once

#include "scopeone/cuda/CudaExport.h"

#include <cstddef>

namespace scopeone::cuda::detail
{
    SCOPEONE_CUDA_EXPORT bool convertToFloat(const void* source,
                                             int sourceStride,
                                             int width,
                                             int height,
                                             int sourceBytesPerPixel,
                                             void* destination,
                                             std::size_t destinationPitchBytes);

    SCOPEONE_CUDA_EXPORT bool convertFromFloat(const void* source,
                                               std::size_t sourcePitchBytes,
                                               void* destination,
                                               int destinationStride,
                                               int width,
                                               int height,
                                               int destinationBytesPerPixel);

    SCOPEONE_CUDA_EXPORT bool launchGaussian(const void* input,
                                             std::size_t inputPitchBytes,
                                             void* output,
                                             std::size_t outputPitchBytes,
                                             int width,
                                             int height,
                                             float sigma);

    SCOPEONE_CUDA_EXPORT bool launchFrequencyFilter(const void* input,
                                                    void* output,
                                                    void* spectrum,
                                                    int width,
                                                    int height,
                                                    float lowCutoff,
                                                    float highCutoff,
                                                    int forwardPlan,
                                                    int inversePlan);
}
