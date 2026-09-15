#pragma once

#include "scopeone/SharedFrame.h"

#include <atomic>
#include <cstring>
#include <limits>

namespace scopeone::core::internal::sharedframe
{
    static_assert(std::atomic_ref<quint32>::is_always_lock_free,
                  "Shared frame state requires lock-free 32-bit atomics");

    inline quint64 payloadSize(const SharedFrameHeader& header)
    {
        return static_cast<quint64>(header.stride) * header.height;
    }

    inline bool headerLooksSane(const SharedFrameHeader& header)
    {
        if (header.channels != 1 || header.width == 0 || header.height == 0
            || header.stride == 0)
        {
            return false;
        }
        const bool mono8 = header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono8);
        const bool mono16 = header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16);
        if ((!mono8 && !mono16)
            || (mono8 && header.bitsPerSample != 8)
            || (mono16 && (header.bitsPerSample == 0 || header.bitsPerSample > 16)))
        {
            return false;
        }
        const quint32 bytesPerPixel = mono16 ? 2u : 1u;
        const quint64 minimumStride = static_cast<quint64>(header.width) * bytesPerPixel;
        const quint64 bytes = payloadSize(header);
        return header.width <= static_cast<quint32>((std::numeric_limits<int>::max)())
            && header.height <= static_cast<quint32>((std::numeric_limits<int>::max)())
            && header.stride <= static_cast<quint32>((std::numeric_limits<int>::max)())
            && header.stride >= minimumStride
            && header.stride % bytesPerPixel == 0
            && bytes > 0
            && bytes <= static_cast<quint64>(kSharedFrameMaxBytes);
    }

    inline bool claimSlot(uchar* slot, SharedFrameHeader& header)
    {
        auto& stateValue = *reinterpret_cast<quint32*>(slot);
        std::atomic_ref<quint32> state(stateValue);
        quint32 expected = 2;
        if (!state.compare_exchange_strong(expected,
                                           3,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire))
        {
            return false;
        }
        std::memcpy(&header, slot, sizeof(header));
        header.state = 2;
        if (headerLooksSane(header)) return true;
        state.store(2, std::memory_order_release);
        return false;
    }

    inline void releaseSlot(uchar* slot)
    {
        auto& stateValue = *reinterpret_cast<quint32*>(slot);
        std::atomic_ref<quint32>(stateValue).store(2, std::memory_order_release);
    }
}
