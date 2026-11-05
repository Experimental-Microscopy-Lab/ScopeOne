#pragma once


#include <cstddef>

namespace scopeone::cuda::detail
{
    bool convertToFloat(const void* source,
                                             int sourceStride,
                                             int width,
                                             int height,
                                             int sourceBytesPerPixel,
                                             void* destination,
                                             std::size_t destinationPitchBytes);

    bool convertFromFloat(const void* source,
                                               std::size_t sourcePitchBytes,
                                               void* destination,
                                               int destinationStride,
                                               int width,
                                               int height,
                                               int destinationBytesPerPixel);

    bool launchGaussian(const void* input,
                                             std::size_t inputPitchBytes,
                                             void* output,
                                             std::size_t outputPitchBytes,
                                             int width,
                                             int height,
                                             int kernelSize,
                                             float sigma);

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
                                                    float maxValue);
}
