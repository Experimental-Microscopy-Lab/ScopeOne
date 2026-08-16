#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <limits>

#include "scopeone/SharedFrame.h"
#include "scopeone/HardwareTypes.h"

namespace scopeone::core
{
    enum class ImagePixelFormat
    {
        Invalid = 0,
        Mono8,
        Mono16
    };

    struct ImageFrame
    {
        QString cameraId;
        int width{0};
        int height{0};
        int stride{0};
        int bitsPerSample{0};
        ImagePixelFormat pixelFormat{ImagePixelFormat::Invalid};
        quint64 frameIndex{0};
        quint64 timestampNs{0};
        ClockStamp clockStamp;
        int sourceRoiX{0};
        int sourceRoiY{0};
        int sourceRoiWidth{0};
        int sourceRoiHeight{0};
        QByteArray bytes;

        static int normalizedBitsPerSample(ImagePixelFormat format, int bitsPerSample)
        {
            if (format == ImagePixelFormat::Mono8)
            {
                return 8;
            }
            if (format == ImagePixelFormat::Mono16)
            {
                return bitsPerSample >= 1 && bitsPerSample <= 16 ? bitsPerSample : 16;
            }
            return 0;
        }

        bool isValid() const
        {
            const qint64 payloadBytes = payloadByteCount();
            const int pixelBytes = bytesPerPixel();
            const qint64 minimumStride = static_cast<qint64>(width) * pixelBytes;
            return width > 0
                && height > 0
                && stride > 0
                && bitsPerSample > 0
                && bitsPerSample == normalizedBitsPerSample(pixelFormat, bitsPerSample)
                && pixelFormat != ImagePixelFormat::Invalid
                && pixelBytes > 0
                && stride >= minimumStride
                && (stride % pixelBytes) == 0
                && payloadBytes > 0
                && bytes.size() == payloadBytes;
        }

        bool isMono8() const
        {
            return pixelFormat == ImagePixelFormat::Mono8;
        }

        bool isMono16() const
        {
            return pixelFormat == ImagePixelFormat::Mono16;
        }

        int bytesPerPixel() const
        {
            if (isMono16())
            {
                return 2;
            }
            if (isMono8())
            {
                return 1;
            }
            return 0;
        }

        qint64 payloadByteCount() const
        {
            if (stride <= 0 || height <= 0)
            {
                return 0;
            }
            const qint64 byteCount = static_cast<qint64>(stride) * static_cast<qint64>(height);
            return byteCount > 0 ? byteCount : 0;
        }

        int maxValue() const
        {
            const int normalizedBits = normalizedBitsPerSample(pixelFormat, bitsPerSample);
            return bitsPerSample == normalizedBits && normalizedBits > 0 ? (1 << normalizedBits) - 1 : 0;
        }

        QSize size() const
        {
            return QSize(width, height);
        }

        bool hasSourceRoi() const
        {
            return sourceRoiWidth > 0 && sourceRoiHeight > 0;
        }

        bool isCompatibleWith(const ImageFrame& other) const
        {
            return width == other.width
                && height == other.height
                && stride == other.stride
                && bitsPerSample == other.bitsPerSample
                && pixelFormat == other.pixelFormat;
        }

        SharedFrameHeader toSharedFrameHeader() const
        {
            SharedFrameHeader header{};
            if (!isValid())
            {
                return header;
            }

            header.state = 2;
            header.width = static_cast<quint32>(width);
            header.height = static_cast<quint32>(height);
            header.stride = static_cast<quint32>(stride);
            header.bitsPerSample = static_cast<quint16>(bitsPerSample);
            header.channels = 1;
            header.frameIndex = frameIndex;
            header.timestampNs = timestampNs;
            header.pixelFormat = pixelFormat == ImagePixelFormat::Mono16
                                     ? static_cast<quint32>(SharedPixelFormat::Mono16)
                                     : static_cast<quint32>(SharedPixelFormat::Mono8);
            if (hasSourceRoi())
            {
                setSharedFrameSourceRoi(header, sourceRoiX, sourceRoiY, sourceRoiWidth, sourceRoiHeight);
            }
            return header;
        }

        static ImageFrame fromSharedFrame(const QString& cameraId,
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
            if (frame.timestampNs > 0)
            {
                frame.clockStamp.ticks = static_cast<std::int64_t>(frame.timestampNs);
                frame.clockStamp.clockDomain = QStringLiteral("provider.timestampNs");
                frame.clockStamp.source = QStringLiteral("Driver");
            }

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
    };
}

Q_DECLARE_METATYPE(scopeone::core::ImageFrame)
