#pragma once

#include "scopeone/ImageFrame.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <limits>

namespace scopeone::core
{
    enum class SharedPixelFormat : quint32
    {
        Mono8 = 0,
        Mono16 = 1
    };


    struct SharedMemoryControl
    {
        quint32 latestSlotIndex;
        quint8 reserved[60];
    };

    static_assert(sizeof(SharedMemoryControl) == 64, "SharedMemoryControl must be 64 bytes");

    struct SharedFrameHeader
    {
        quint32 state;
        quint32 width;
        quint32 height;
        quint32 stride;
        quint32 pixelFormat;
        quint16 bitsPerSample;
        quint16 channels;
        quint64 frameIndex;
        quint64 timestampNs;
        qint32 sourceRoiX;
        qint32 sourceRoiY;
        qint32 sourceRoiWidth;
        qint32 sourceRoiHeight;
        quint32 sourceRoiValid;
        quint8 reserved[64 - (4 + 4 + 4 + 4 + 4 + 2 + 2 + 8 + 8 + 4 + 4 + 4 + 4 + 4)];
    };

    static_assert(sizeof(SharedFrameHeader) == 64, "SharedFrameHeader must be 64 bytes");

    inline bool sharedFrameHasSourceRoi(const SharedFrameHeader& header)
    {
        return header.sourceRoiValid != 0
            && header.sourceRoiWidth > 0
            && header.sourceRoiHeight > 0;
    }

    inline void setSharedFrameSourceRoi(SharedFrameHeader& header, int x, int y, int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            header.sourceRoiX = 0;
            header.sourceRoiY = 0;
            header.sourceRoiWidth = 0;
            header.sourceRoiHeight = 0;
            header.sourceRoiValid = 0;
            return;
        }

        header.sourceRoiX = static_cast<qint32>(x);
        header.sourceRoiY = static_cast<qint32>(y);
        header.sourceRoiWidth = static_cast<qint32>(width);
        header.sourceRoiHeight = static_cast<qint32>(height);
        header.sourceRoiValid = 1;
    }

    inline quint64 computeMaxFrameBytes(quint32 width, quint32 height, SharedPixelFormat fmt)
    {
        switch (fmt)
        {
        case SharedPixelFormat::Mono8: return static_cast<quint64>(width) * height * 1u;
        case SharedPixelFormat::Mono16: return static_cast<quint64>(width) * height * 2u;
        default: return 0;
        }
    }

    inline constexpr int kSharedFrameNumSlots = 32;
    inline constexpr int kSharedFrameMaxBytes = 4 * 2048 * 2048;
    inline constexpr int kSharedMemoryControlSize = 64;
    inline constexpr int kSharedFrameHeaderSize = static_cast<int>(sizeof(SharedFrameHeader));
    inline constexpr int kSharedFrameSlotStride = kSharedFrameHeaderSize + kSharedFrameMaxBytes;

    // The header of a frame in the DriverHost frame ring
    inline SharedFrameHeader toSharedFrameHeader(const ImageFrame& frame)
    {
        SharedFrameHeader header{};
        if (!frame.isValid())
        {
            return header;
        }

        header.state = 2;
        header.width = static_cast<quint32>(frame.width);
        header.height = static_cast<quint32>(frame.height);
        header.stride = static_cast<quint32>(frame.stride);
        header.bitsPerSample = static_cast<quint16>(frame.bitsPerSample);
        header.channels = 1;
        header.frameIndex = frame.frameIndex;
        header.timestampNs = frame.timestampNs;
        header.pixelFormat = frame.pixelFormat == ImagePixelFormat::Mono16
                                 ? static_cast<quint32>(SharedPixelFormat::Mono16)
                                 : static_cast<quint32>(SharedPixelFormat::Mono8);
        if (frame.hasSourceRoi())
        {
            setSharedFrameSourceRoi(header, frame.sourceRoiX, frame.sourceRoiY, frame.sourceRoiWidth, frame.sourceRoiHeight);
        }
        return header;
    }

    // A frame read back from the DriverHost frame ring, or an invalid frame when the header does not fit
    inline ImageFrame imageFrameFromShared(const QString& cameraId,
                                           const SharedFrameHeader& header,
                                           const QByteArray& payload)
    {
        if (header.state != 2
            || header.channels != 1
            || header.width > static_cast<quint32>((std::numeric_limits<int>::max)())
            || header.height > static_cast<quint32>((std::numeric_limits<int>::max)())
            || header.stride > static_cast<quint32>((std::numeric_limits<int>::max)()))
        {
            return {};
        }

        ImageFrame frame;
        frame.cameraId = cameraId.trimmed();
        frame.width = static_cast<int>(header.width);
        frame.height = static_cast<int>(header.height);
        frame.bitsPerSample = static_cast<int>(header.bitsPerSample);
        frame.frameIndex = header.frameIndex;
        frame.timestampNs = header.timestampNs;

        if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16))
        {
            if (header.bitsPerSample == 0 || header.bitsPerSample > 16)
            {
                return {};
            }
            frame.pixelFormat = ImagePixelFormat::Mono16;
        }
        else if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono8))
        {
            if (header.bitsPerSample != 8)
            {
                return {};
            }
            frame.pixelFormat = ImagePixelFormat::Mono8;
        }
        else
        {
            return {};
        }

        frame.stride = static_cast<int>(header.stride);
        frame.bytes = payload;
        if (sharedFrameHasSourceRoi(header))
        {
            frame.sourceRoiX = static_cast<int>(header.sourceRoiX);
            frame.sourceRoiY = static_cast<int>(header.sourceRoiY);
            frame.sourceRoiWidth = static_cast<int>(header.sourceRoiWidth);
            frame.sourceRoiHeight = static_cast<int>(header.sourceRoiHeight);
        }
        return frame.isValid() ? frame : ImageFrame{};
    }
}
