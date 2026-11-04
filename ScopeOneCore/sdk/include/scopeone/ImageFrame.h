#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <limits>


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

    };
}

Q_DECLARE_METATYPE(scopeone::core::ImageFrame)
