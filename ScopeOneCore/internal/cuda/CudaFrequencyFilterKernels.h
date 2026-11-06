#pragma once

#include <cstddef>

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
                                                    float maxValue);
}
