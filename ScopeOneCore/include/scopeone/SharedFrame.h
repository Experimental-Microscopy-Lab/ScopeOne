#pragma once

#include <QtGlobal>
#include <QMetaType>

namespace scopeone::core {

enum class SharedPixelFormat : quint32 {
    Mono8  = 0,
    Mono16 = 1
};


struct SharedMemoryControl {
    quint32 latestSlotIndex;
    quint8 reserved[60];
};

static_assert(sizeof(SharedMemoryControl) == 64, "SharedMemoryControl must be 64 bytes");

struct SharedFrameHeader {
    quint32 state;
    quint32 width;
    quint32 height;
    quint32 stride;
    quint32 pixelFormat;
    quint16 bitsPerSample;
    quint16 channels;
    quint64 frameIndex;
    quint64 timestampNs;
    quint8  reserved[64 - (4+4+4+4+4+2+2+8+8)];
};

static_assert(sizeof(SharedFrameHeader) == 64, "SharedFrameHeader must be 64 bytes");

inline quint64 computeMaxFrameBytes(quint32 width, quint32 height, SharedPixelFormat fmt) {
    switch (fmt) {
        case SharedPixelFormat::Mono8:  return static_cast<quint64>(width) * height * 1u;
        case SharedPixelFormat::Mono16: return static_cast<quint64>(width) * height * 2u;
        default: return 0;
    }
}

inline constexpr int kSharedFrameNumSlots = 12;
inline constexpr int kSharedFrameMaxBytes = 16 * 2048 * 2048;
inline constexpr int kSharedMemoryControlSize = 64;
inline constexpr int kSharedFrameHeaderSize = static_cast<int>(sizeof(SharedFrameHeader));
inline constexpr int kSharedFrameSlotStride = kSharedFrameHeaderSize + kSharedFrameMaxBytes;

}

Q_DECLARE_METATYPE(scopeone::core::SharedFrameHeader)


