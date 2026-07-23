#include "internal/FrameBufferUtils.h"

#include <cstring>
#include <limits>
#include <utility>
#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;

    // Copies source identity and acquisition metadata to an output frame
    void copyFrameMetadata(const ImageFrame& src, ImageFrame& dst)
    {
        dst.cameraId = src.cameraId;
        dst.frameIndex = src.frameIndex;
        dst.timestampNs = src.timestampNs;
        dst.sourceRoiX = src.sourceRoiX;
        dst.sourceRoiY = src.sourceRoiY;
        dst.sourceRoiWidth = src.sourceRoiWidth;
        dst.sourceRoiHeight = src.sourceRoiHeight;
    }

    namespace
    {
        // Converts a sixteen bit sample to an eight bit display value
        int mono8ValueFrom16(int value, int bitsPerSample)
        {
            const int clampedBits = qBound(1, bitsPerSample, 16);
            if (clampedBits <= 8)
            {
                const int maxIn = (1 << clampedBits) - 1;
                return qBound(0, (value * 255 + maxIn / 2) / maxIn, 255);
            }

            const int shift = clampedBits - 8;
            const int rounded = value + (1 << (shift - 1));
            return qBound(0, rounded >> shift, 255);
        }
    } // namespace

    // Converts a valid mono frame to mono eight format
    bool convertFrameToMono8(const ImageFrame& src, ImageFrame& dst)
    {
        if (!src.isValid())
        {
            return false;
        }

        if (src.isMono8())
        {
            dst = src;
            return true;
        }

        if (!src.isMono16())
        {
            return false;
        }

        QByteArray bytes = allocatePixelBytes<uchar>(src.width, src.height);
        if (bytes.isEmpty())
        {
            return false;
        }
        uchar* dstData = reinterpret_cast<uchar*>(bytes.data());
        const char* srcData = src.bytes.constData();

        parallelForImageRows(src.width, src.height, [&](int firstRow, int lastRow)
        {
            for (int y = firstRow; y < lastRow; ++y)
            {
                const quint16* srcRow = reinterpret_cast<const quint16*>(
                    srcData + static_cast<qint64>(y) * src.stride);
                uchar* dstRow = dstData + static_cast<qint64>(y) * src.width;
                for (int x = 0; x < src.width; ++x)
                {
                    dstRow[x] = static_cast<uchar>(
                        mono8ValueFrom16(static_cast<int>(srcRow[x]), src.bitsPerSample));
                }
            }
        });

        dst = makeMono8Frame(src.cameraId, src.width, src.height, std::move(bytes));
        copyFrameMetadata(src, dst);
        return dst.isValid();
    }

    // Converts a frame to the requested processing bit depth
    bool convertFrameForProcessing(const ImageFrame& src, ImageFrame& dst, int processingBitDepth)
    {
        const int bitDepth = processingBitDepth >= 16 ? 16 : 8;
        if (!src.isValid())
        {
            return false;
        }

        if (bitDepth == 8)
        {
            return convertFrameToMono8(src, dst);
        }

        if (src.isMono8() || src.isMono16())
        {
            dst = src;
            return true;
        }
        return false;
    }

    // Builds a mono eight frame from owned pixel bytes
    ImageFrame makeMono8Frame(const QString& cameraId,
                              int width,
                              int height,
                              QByteArray bytes)
    {
        ImageFrame frame;
        frame.cameraId = cameraId;
        frame.width = width;
        frame.height = height;
        frame.stride = width;
        frame.bitsPerSample = 8;
        frame.pixelFormat = ImagePixelFormat::Mono8;
        frame.bytes = std::move(bytes);
        return frame;
    }

    // Builds a mono sixteen frame from owned pixel bytes
    ImageFrame makeMono16Frame(const QString& cameraId,
                               int width,
                               int height,
                               QByteArray bytes,
                               int bitsPerSample)
    {
        ImageFrame frame;
        frame.cameraId = cameraId;
        frame.width = width;
        frame.height = height;
        const qint64 stride = static_cast<qint64>(width) * static_cast<qint64>(sizeof(quint16));
        if (stride <= 0 || stride > (std::numeric_limits<int>::max)())
        {
            return frame;
        }
        frame.stride = static_cast<int>(stride);
        frame.pixelFormat = ImagePixelFormat::Mono16;
        frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(frame.pixelFormat, bitsPerSample);
        frame.bytes = std::move(bytes);
        return frame;
    }

    // Builds an output frame with the same pixel format as a reference frame
    ImageFrame makeFrameLike(const ImageFrame& reference,
                             int width,
                             int height,
                             QByteArray bytes)
    {
        if (reference.isMono16())
        {
            ImageFrame frame = makeMono16Frame(reference.cameraId,
                                               width,
                                               height,
                                               std::move(bytes),
                                               reference.bitsPerSample);
            copyFrameMetadata(reference, frame);
            return frame;
        }
        ImageFrame frame = makeMono8Frame(reference.cameraId, width, height, std::move(bytes));
        copyFrameMetadata(reference, frame);
        return frame;
    }

    // Copies contiguous OpenCV matrix rows into a byte array
    QByteArray copyMatBytes(const cv::Mat& mat)
    {
        QByteArray bytes;
        const qint64 rowBytes = static_cast<qint64>(mat.cols) * static_cast<qint64>(mat.elemSize());
        const qint64 byteCount = static_cast<qint64>(rowBytes) * static_cast<qint64>(mat.rows);
        if (rowBytes <= 0
            || mat.rows <= 0
            || byteCount <= 0
            || byteCount > (std::numeric_limits<qsizetype>::max)())
        {
            return bytes;
        }
        bytes.resize(static_cast<qsizetype>(byteCount));
        for (int y = 0; y < mat.rows; ++y)
        {
            std::memcpy(bytes.data() + static_cast<qint64>(y) * rowBytes,
                        mat.ptr(y),
                        static_cast<size_t>(rowBytes));
        }
        return bytes;
    }
} // namespace scopeone::core::internal
