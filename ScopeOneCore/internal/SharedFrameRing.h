#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/SharedFrame.h"

#include <QList>
#include <QSharedMemory>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <vector>

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

    // Attaches the frame ring of one camera, which must be large enough for all slots
    inline bool attachRing(QSharedMemory& shm)
    {
        const int expectedSize = kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
        if (!shm.isAttached() && !shm.attach(QSharedMemory::ReadWrite))
        {
            return false;
        }
        if (shm.size() >= expectedSize)
        {
            return true;
        }
        shm.detach();
        return false;
    }

    inline uchar* ringSlot(uchar* base, int index)
    {
        return base + kSharedMemoryControlSize + index * kSharedFrameSlotStride;
    }

    // Copies the pixels of one claimed slot into a frame of its own
    inline bool copySlotFrame(const QString& cameraId,
                              const SharedFrameHeader& header,
                              const uchar* slot,
                              QList<ImageFrame>& frames)
    {
        QByteArray payload(static_cast<qsizetype>(payloadSize(header)), Qt::Uninitialized);
        std::memcpy(payload.data(), slot + kSharedFrameHeaderSize, static_cast<size_t>(payload.size()));
        ImageFrame frame = imageFrameFromShared(cameraId, header, payload);
        if (!frame.isValid())
        {
            return false;
        }
        frames.append(std::move(frame));
        return true;
    }

    // Copies the newest frame after lastFrameIndex and moves lastFrameIndex to it. The slot
    // the writer marked as latest is tried first, then every slot.
    inline bool readLatestFrame(uchar* base,
                                const QString& cameraId,
                                quint64& lastFrameIndex,
                                QList<ImageFrame>& frames)
    {
        // Xcode 16's libc++ fails to compile std::atomic_ref<const T>::load, so the load goes through a non-const reference
        auto* control = reinterpret_cast<SharedMemoryControl*>(base);
        SharedFrameHeader capturedHeader{};
        uchar* capturedSlot = nullptr;
        const quint32 latest = std::atomic_ref<quint32>(control->latestSlotIndex)
                                   .load(std::memory_order_acquire);
        if (latest < kSharedFrameNumSlots)
        {
            uchar* slot = ringSlot(base, static_cast<int>(latest));
            SharedFrameHeader header{};
            if (claimSlot(slot, header))
            {
                if (header.frameIndex > lastFrameIndex)
                {
                    capturedHeader = header;
                    capturedSlot = slot;
                }
                else
                {
                    releaseSlot(slot);
                }
            }
        }
        if (!capturedSlot)
        {
            quint64 bestIndex = lastFrameIndex;
            for (int index = 0; index < kSharedFrameNumSlots; ++index)
            {
                uchar* slot = ringSlot(base, index);
                SharedFrameHeader header{};
                if (!claimSlot(slot, header))
                {
                    continue;
                }
                if (header.frameIndex > bestIndex)
                {
                    if (capturedSlot)
                    {
                        releaseSlot(capturedSlot);
                    }
                    bestIndex = header.frameIndex;
                    capturedHeader = header;
                    capturedSlot = slot;
                }
                else
                {
                    releaseSlot(slot);
                }
            }
        }
        if (!capturedSlot)
        {
            return false;
        }
        const bool copied = copySlotFrame(cameraId, capturedHeader, capturedSlot, frames);
        releaseSlot(capturedSlot);
        if (copied)
        {
            lastFrameIndex = capturedHeader.frameIndex;
        }
        return copied;
    }

    // Copies every frame after lastFrameIndex in order and moves lastFrameIndex to the last one
    inline bool readNewFrames(uchar* base,
                              const QString& cameraId,
                              quint64& lastFrameIndex,
                              QList<ImageFrame>& frames)
    {
        struct Claimed
        {
            uchar* slot{nullptr};
            SharedFrameHeader header{};
        };
        std::vector<Claimed> claimed;
        claimed.reserve(kSharedFrameNumSlots);
        for (int index = 0; index < kSharedFrameNumSlots; ++index)
        {
            uchar* slot = ringSlot(base, index);
            SharedFrameHeader header{};
            if (!claimSlot(slot, header))
            {
                continue;
            }
            if (header.frameIndex > lastFrameIndex)
            {
                claimed.push_back({slot, header});
            }
            else
            {
                releaseSlot(slot);
            }
        }
        std::sort(claimed.begin(), claimed.end(), [](const Claimed& lhs, const Claimed& rhs)
        {
            return lhs.header.frameIndex < rhs.header.frameIndex;
        });
        bool copiedAny = false;
        for (const Claimed& item : claimed)
        {
            if (copySlotFrame(cameraId, item.header, item.slot, frames))
            {
                lastFrameIndex = item.header.frameIndex;
                copiedAny = true;
            }
            releaseSlot(item.slot);
        }
        return copiedAny;
    }

    // Creates the frame ring of one camera with every slot empty. A ring of the same name
    // that a crashed DriverHost left behind is dropped first.
    inline bool createRing(QSharedMemory& shm, QString* errorMessage)
    {
        const int totalBytes = kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
        if (!shm.create(totalBytes))
        {
            if (shm.attach())
            {
                shm.detach();
            }
            if (!shm.create(totalBytes))
            {
                *errorMessage = QStringLiteral("Cannot create shared memory '%1'").arg(shm.nativeKey());
                return false;
            }
        }
        if (!shm.lock())
        {
            *errorMessage = QStringLiteral("Cannot initialize shared memory '%1'").arg(shm.nativeKey());
            return false;
        }
        auto* const base = static_cast<uchar*>(shm.data());
        const SharedMemoryControl control{};
        std::memcpy(base, &control, sizeof(control));
        for (int index = 0; index < kSharedFrameNumSlots; ++index)
        {
            const SharedFrameHeader header{};
            std::memcpy(ringSlot(base, index), &header, sizeof(header));
        }
        shm.unlock();
        return true;
    }

    // Writes one frame into the first free slot from nextWriteSlot on and marks it as the
    // latest. False when every slot is being read.
    inline bool writeFrame(uchar* base,
                           int& nextWriteSlot,
                           const SharedFrameHeader& header,
                           const void* pixels,
                           std::size_t byteCount)
    {
        int slotIndex = -1;
        uchar* slot = nullptr;
        for (int offset = 0; offset < kSharedFrameNumSlots && !slot; ++offset)
        {
            const int candidate = (nextWriteSlot + offset) % kSharedFrameNumSlots;
            uchar* const candidateSlot = ringSlot(base, candidate);
            std::atomic_ref<quint32> state(*reinterpret_cast<quint32*>(candidateSlot));
            quint32 expected = state.load(std::memory_order_acquire);
            while (expected == 0 || expected == 2)
            {
                if (state.compare_exchange_weak(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    slotIndex = candidate;
                    slot = candidateSlot;
                    break;
                }
            }
        }
        if (!slot)
        {
            return false;
        }

        nextWriteSlot = (slotIndex + 1) % kSharedFrameNumSlots;
        // The state word stays 1 until the pixels are in place
        std::memcpy(slot + sizeof(header.state),
                    reinterpret_cast<const uchar*>(&header) + sizeof(header.state),
                    sizeof(header) - sizeof(header.state));
        std::memcpy(slot + kSharedFrameHeaderSize, pixels, byteCount);
        std::atomic_ref<quint32>(*reinterpret_cast<quint32*>(slot)).store(2, std::memory_order_release);
        auto* const control = reinterpret_cast<SharedMemoryControl*>(base);
        std::atomic_ref<quint32>(control->latestSlotIndex)
            .store(static_cast<quint32>(slotIndex), std::memory_order_release);
        return true;
    }
}
