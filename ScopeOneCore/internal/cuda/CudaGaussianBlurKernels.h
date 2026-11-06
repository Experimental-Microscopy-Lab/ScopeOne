#pragma once

#include <cstddef>

namespace scopeone::core::internal::cuda::detail
{
    bool launchGaussian(const void* input,
                                             std::size_t inputPitchBytes,
                                             void* output,
                                             std::size_t outputPitchBytes,
                                             int width,
                                             int height,
                                             int kernelSize,
                                             float sigma);
}
