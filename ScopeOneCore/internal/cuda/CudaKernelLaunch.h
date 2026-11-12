#pragma once


#include <cstddef>

namespace scopeone::core::internal::cuda::detail
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
                                               int destinationBytesPerPixel,
                                               float scale,
                                               float offset);

    // Checks the last launch and waits for the device, so a failed kernel is reported at once
    bool synchronizeKernel();
}
