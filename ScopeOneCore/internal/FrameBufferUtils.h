#pragma once

#include "scopeone/ImageFrame.h"

#include <utility>

namespace cv
{
    class Mat;
}

namespace scopeone::core::internal
{
    bool convertFrameToMono8(const scopeone::core::ImageFrame& src,
                             scopeone::core::ImageFrame& dst);

    bool convertFrameForProcessing(const scopeone::core::ImageFrame& src,
                                   scopeone::core::ImageFrame& dst,
                                   int processingBitDepth);

    scopeone::core::ImageFrame makeMono8Frame(const QString& cameraId,
                                              int width,
                                              int height,
                                              QByteArray bytes);

    scopeone::core::ImageFrame makeMono16Frame(const QString& cameraId,
                                               int width,
                                               int height,
                                               QByteArray bytes,
                                               int bitsPerSample = 16);

    scopeone::core::ImageFrame makeFrameLike(const scopeone::core::ImageFrame& reference,
                                             int width,
                                             int height,
                                             QByteArray bytes);

    QByteArray copyMatBytes(const cv::Mat& mat);

    template <typename Pixel>
    inline QByteArray allocatePixelBytes(int width, int height)
    {
        QByteArray bytes;
        bytes.resize(width * height * static_cast<int>(sizeof(Pixel)));
        return bytes;
    }

    template <typename Pixel>
    inline const Pixel* frameRowData(const scopeone::core::ImageFrame& frame, int y)
    {
        return reinterpret_cast<const Pixel*>(frame.bytes.constData() + y * frame.stride);
    }

    template <typename Pixel>
    inline Pixel* mutableRowData(QByteArray& bytes, int width, int y)
    {
        return reinterpret_cast<Pixel*>(bytes.data()) + y * width;
    }

    template <typename Pixel>
    inline Pixel clampPixelValue(int value, int maxValue)
    {
        return static_cast<Pixel>(qBound(0, value, maxValue));
    }

    template <typename Handler>
    decltype(auto) dispatchFrameType(const scopeone::core::ImageFrame& frame, Handler&& handler)
    {
        if (frame.isMono16())
        {
            return std::forward<Handler>(handler).template operator()<quint16>();
        }
        return std::forward<Handler>(handler).template operator()<uchar>();
    }
}
