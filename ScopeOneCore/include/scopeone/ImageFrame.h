#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QSize>
#include <QString>

#include "scopeone/SharedFrame.h"

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
        int sourceRoiX{0};
        int sourceRoiY{0};
        int sourceRoiWidth{0};
        int sourceRoiHeight{0};
        QByteArray bytes;

        bool isValid() const
        {
            return width > 0
                && height > 0
                && stride > 0
                && bitsPerSample > 0
                && pixelFormat != ImagePixelFormat::Invalid
                && bytes.size() >= stride * height;
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

        int maxValue() const
        {
            const int clampedBits = qBound(1, bitsPerSample, 16);
            return (1 << clampedBits) - 1;
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

        static ImageFrame fromSharedFrame(const QString& cameraId,
                                          const SharedFrameHeader& header,
                                          const QByteArray& rawData)
        {
            ImageFrame frame;
            frame.cameraId = cameraId;
            frame.width = static_cast<int>(header.width);
            frame.height = static_cast<int>(header.height);
            frame.bitsPerSample = static_cast<int>(header.bitsPerSample);

            if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16))
            {
                frame.pixelFormat = ImagePixelFormat::Mono16;
                if (frame.bitsPerSample <= 0)
                {
                    frame.bitsPerSample = 16;
                }
            }
            else if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono8))
            {
                frame.pixelFormat = ImagePixelFormat::Mono8;
                frame.bitsPerSample = 8;
            }
            else
            {
                return {};
            }

            const int bytesPerPixel = frame.bytesPerPixel();
            frame.stride = static_cast<int>(header.stride);
            if (frame.stride <= 0 && frame.width > 0)
            {
                frame.stride = frame.width * bytesPerPixel;
            }
            frame.bytes = rawData;
            if (sharedFrameHasSourceRoi(header))
            {
                frame.sourceRoiX = static_cast<int>(header.sourceRoiX);
                frame.sourceRoiY = static_cast<int>(header.sourceRoiY);
                frame.sourceRoiWidth = static_cast<int>(header.sourceRoiWidth);
                frame.sourceRoiHeight = static_cast<int>(header.sourceRoiHeight);
            }
            else
            {
                frame.sourceRoiX = 0;
                frame.sourceRoiY = 0;
                frame.sourceRoiWidth = frame.width;
                frame.sourceRoiHeight = frame.height;
            }
            return frame.isValid() ? frame : ImageFrame{};
        }
    };
}

Q_DECLARE_METATYPE(scopeone::core::ImageFrame)
