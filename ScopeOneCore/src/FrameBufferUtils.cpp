#include "internal/FrameBufferUtils.h"

namespace scopeone::core::internal {

using scopeone::core::ImageFrame;
using scopeone::core::ImagePixelFormat;

namespace {

int mono8ValueFrom16(int value, int bitsPerSample)
{
    const int clampedBits = qBound(1, bitsPerSample, 16);
    if (clampedBits <= 8) {
        const int maxIn = (1 << clampedBits) - 1;
        if (maxIn <= 0) {
            return 0;
        }
        return qBound(0, (value * 255 + maxIn / 2) / maxIn, 255);
    }

    const int shift = clampedBits - 8;
    const int rounded = value + (1 << (shift - 1));
    return qBound(0, rounded >> shift, 255);
}

} // namespace

bool convertFrameToMono8(const ImageFrame& src, ImageFrame& dst)
{
    // Normalize supported input formats to mono8
    if (!src.isValid()) {
        return false;
    }

    if (src.isMono8()) {
        dst = src;
        return true;
    }

    if (!src.isMono16()) {
        return false;
    }

    QByteArray bytes;
    bytes.resize(src.width * src.height);
    uchar* dstData = reinterpret_cast<uchar*>(bytes.data());
    const char* srcData = src.bytes.constData();

    for (int y = 0; y < src.height; ++y) {
        const quint16* srcRow = reinterpret_cast<const quint16*>(srcData + y * src.stride);
        uchar* dstRow = dstData + y * src.width;
        for (int x = 0; x < src.width; ++x) {
            dstRow[x] = static_cast<uchar>(mono8ValueFrom16(static_cast<int>(srcRow[x]),
                                                            src.bitsPerSample));
        }
    }

    dst = makeMono8Frame(src.cameraId, src.width, src.height, std::move(bytes));
    return dst.isValid();
}

ImageFrame makeMono8Frame(const QString& cameraId,
                          int width,
                          int height,
                          QByteArray bytes)
{
    // Build a compact mono8 frame container
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

} // namespace scopeone::core::internal
