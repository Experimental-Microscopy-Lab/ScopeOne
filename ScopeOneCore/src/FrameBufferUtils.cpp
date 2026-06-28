#include "internal/FrameBufferUtils.h"

#include <cstring>
#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;

    namespace
    {
        void copySourceRoi(const ImageFrame& src, ImageFrame& dst)
        {
            dst.sourceRoiX = src.sourceRoiX;
            dst.sourceRoiY = src.sourceRoiY;
            dst.sourceRoiWidth = src.sourceRoiWidth;
            dst.sourceRoiHeight = src.sourceRoiHeight;
        }

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

        QByteArray bytes;
        bytes.resize(src.width * src.height);
        uchar* dstData = reinterpret_cast<uchar*>(bytes.data());
        const char* srcData = src.bytes.constData();

        for (int y = 0; y < src.height; ++y)
        {
            const quint16* srcRow = reinterpret_cast<const quint16*>(srcData + y * src.stride);
            uchar* dstRow = dstData + y * src.width;
            for (int x = 0; x < src.width; ++x)
            {
                dstRow[x] = static_cast<uchar>(mono8ValueFrom16(static_cast<int>(srcRow[x]),
                                                                src.bitsPerSample));
            }
        }

        dst = makeMono8Frame(src.cameraId, src.width, src.height, std::move(bytes));
        copySourceRoi(src, dst);
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
        frame.stride = width * static_cast<int>(sizeof(quint16));
        frame.bitsPerSample = qBound(9, bitsPerSample, 16);
        frame.pixelFormat = ImagePixelFormat::Mono16;
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
                                               reference.bitsPerSample > 8 ? reference.bitsPerSample : 16);
            copySourceRoi(reference, frame);
            return frame;
        }
        ImageFrame frame = makeMono8Frame(reference.cameraId, width, height, std::move(bytes));
        copySourceRoi(reference, frame);
        return frame;
    }

    // Copies contiguous OpenCV matrix rows into a byte array
    QByteArray copyMatBytes(const cv::Mat& mat)
    {
        QByteArray bytes;
        const int rowBytes = mat.cols * static_cast<int>(mat.elemSize());
        bytes.resize(rowBytes * mat.rows);
        for (int y = 0; y < mat.rows; ++y)
        {
            std::memcpy(bytes.data() + y * rowBytes, mat.ptr(y), static_cast<size_t>(rowBytes));
        }
        return bytes;
    }
} // namespace scopeone::core::internal
