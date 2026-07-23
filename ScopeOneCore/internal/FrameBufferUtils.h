#pragma once

#include "scopeone/ImageFrame.h"

#include <limits>
#include <opencv2/core/utility.hpp>
#include <utility>

namespace cv
{
    class Mat;
}

namespace scopeone::core::internal
{
    void copyFrameMetadata(const scopeone::core::ImageFrame& src,
                           scopeone::core::ImageFrame& dst);

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
        const qint64 byteCount = static_cast<qint64>(width)
            * static_cast<qint64>(height)
            * static_cast<qint64>(sizeof(Pixel));
        if (width <= 0
            || height <= 0
            || byteCount <= 0
            || byteCount > (std::numeric_limits<qsizetype>::max)())
        {
            return bytes;
        }
        bytes.resize(static_cast<qsizetype>(byteCount));
        return bytes;
    }

    template <typename Pixel>
    inline const Pixel* frameRowData(const scopeone::core::ImageFrame& frame, int y)
    {
        return reinterpret_cast<const Pixel*>(frame.bytes.constData() + static_cast<qint64>(y) * frame.stride);
    }

    template <typename Pixel>
    inline Pixel clampPixelValue(int value, int maxValue)
    {
        return static_cast<Pixel>(qBound(0, value, maxValue));
    }

    template <typename Handler>
    inline void parallelForRows(qint64 workItemCount, int rowCount, Handler&& handler)
    {
        constexpr qint64 kMinimumParallelWorkItems = 1024ll * 1024ll;
        if (rowCount <= 0 || workItemCount < kMinimumParallelWorkItems)
        {
            handler(0, qMax(0, rowCount));
            return;
        }

        cv::parallel_for_(cv::Range(0, rowCount),
                          [&handler](const cv::Range& range)
                          {
                              handler(range.start, range.end);
                          });
    }

    template <typename Handler>
    inline void parallelForImageRows(int width, int height, Handler&& handler)
    {
        parallelForRows(static_cast<qint64>(width) * height,
                        height,
                        std::forward<Handler>(handler));
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
