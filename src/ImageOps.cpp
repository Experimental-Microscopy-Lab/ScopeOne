#include "ImageOps.h"

#include <QSize>

#include <limits>
#include <numeric>

namespace scopeone::ui::imageops
{
    using scopeone::core::ImagePixelFormat;

    namespace
    {
        // Offsets of a disc around a pixel, and for a ball the height of the ball above each
        struct Offset
        {
            int dx;
            int dy;
            float height;
        };

        std::vector<Offset> discOffsets(double radius)
        {
            std::vector<Offset> offsets;
            const int reach = static_cast<int>(std::floor(radius));
            const double limit = radius * radius + 1.0;
            for (int dy = -reach; dy <= reach; ++dy)
            {
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    if (dx * dx + dy * dy <= limit - 1.0 + 0.5)
                    {
                        offsets.push_back({dx, dy, 0.0f});
                    }
                }
            }
            if (offsets.empty())
            {
                offsets.push_back({0, 0, 0.0f});
            }
            return offsets;
        }

        std::vector<Offset> ballOffsets(double radius)
        {
            std::vector<Offset> offsets;
            const int reach = static_cast<int>(std::floor(radius));
            for (int dy = -reach; dy <= reach; ++dy)
            {
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    const double squared = radius * radius - dx * dx - dy * dy;
                    if (squared >= 0.0)
                    {
                        offsets.push_back({dx, dy, static_cast<float>(std::sqrt(squared))});
                    }
                }
            }
            return offsets;
        }

        inline int clampIndex(int value, int low, int high)
        {
            return value < low ? low : (value > high ? high : value);
        }

        // Smallest or largest of the values under the offsets. The heights of a ball lower the
        // values for the smallest and raise them for the largest.
        std::vector<float> morphology(const std::vector<float>& source,
                                      int width,
                                      int height,
                                      const std::vector<Offset>& offsets,
                                      bool minimum)
        {
            std::vector<float> result(source.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float best = minimum ? std::numeric_limits<float>::max()
                                         : std::numeric_limits<float>::lowest();
                    for (const Offset& offset : offsets)
                    {
                        const int sx = clampIndex(x + offset.dx, 0, width - 1);
                        const int sy = clampIndex(y + offset.dy, 0, height - 1);
                        const float value = source[static_cast<size_t>(sy) * width + sx];
                        best = minimum ? std::min(best, value - offset.height)
                                       : std::max(best, value + offset.height);
                    }
                    result[static_cast<size_t>(y) * width + x] = best;
                }
            }
            return result;
        }

        std::vector<float> gaussianKernel(double sigma)
        {
            const int reach = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
            std::vector<float> kernel(static_cast<size_t>(2 * reach + 1));
            float sum = 0.0f;
            for (int i = -reach; i <= reach; ++i)
            {
                const float weight = static_cast<float>(std::exp(-(i * i) / (2.0 * sigma * sigma)));
                kernel[static_cast<size_t>(i + reach)] = weight;
                sum += weight;
            }
            for (float& weight : kernel)
            {
                weight /= sum;
            }
            return kernel;
        }

        // Smooths along x and then along y with the same kernel, repeating the edge pixels
        std::vector<float> separableBlur(const std::vector<float>& source,
                                         int width,
                                         int height,
                                         const std::vector<float>& kernel)
        {
            const int reach = static_cast<int>(kernel.size() / 2);
            std::vector<float> horizontal(source.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float sum = 0.0f;
                    for (int k = -reach; k <= reach; ++k)
                    {
                        sum += kernel[static_cast<size_t>(k + reach)]
                            * source[static_cast<size_t>(y) * width + clampIndex(x + k, 0, width - 1)];
                    }
                    horizontal[static_cast<size_t>(y) * width + x] = sum;
                }
            }
            std::vector<float> result(source.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float sum = 0.0f;
                    for (int k = -reach; k <= reach; ++k)
                    {
                        sum += kernel[static_cast<size_t>(k + reach)]
                            * horizontal[static_cast<size_t>(clampIndex(y + k, 0, height - 1)) * width + x];
                    }
                    result[static_cast<size_t>(y) * width + x] = sum;
                }
            }
            return result;
        }

        std::vector<float> boxKernel(double radius)
        {
            const int reach = std::max(1, static_cast<int>(std::round(radius)));
            return std::vector<float>(static_cast<size_t>(2 * reach + 1), 1.0f / (2 * reach + 1));
        }

        float sampleBilinear(const std::vector<float>& source, int width, int height, double x, double y)
        {
            const double fx = std::clamp(x - 0.5, 0.0, static_cast<double>(width - 1));
            const double fy = std::clamp(y - 0.5, 0.0, static_cast<double>(height - 1));
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const int x1 = std::min(x0 + 1, width - 1);
            const int y1 = std::min(y0 + 1, height - 1);
            const double tx = fx - x0;
            const double ty = fy - y0;
            const auto at = [&](int ix, int iy) { return source[static_cast<size_t>(iy) * width + ix]; };
            return static_cast<float>((1 - ty) * ((1 - tx) * at(x0, y0) + tx * at(x1, y0))
                                      + ty * ((1 - tx) * at(x0, y1) + tx * at(x1, y1)));
        }

        std::vector<float> resampled(const std::vector<float>& source,
                                     int width,
                                     int height,
                                     int newWidth,
                                     int newHeight,
                                     bool smooth)
        {
            std::vector<float> result(static_cast<size_t>(newWidth) * newHeight);
            for (int y = 0; y < newHeight; ++y)
            {
                const double sy = (y + 0.5) * height / newHeight;
                for (int x = 0; x < newWidth; ++x)
                {
                    const double sx = (x + 0.5) * width / newWidth;
                    result[static_cast<size_t>(y) * newWidth + x] =
                        smooth ? sampleBilinear(source, width, height, sx, sy)
                               : source[static_cast<size_t>(clampIndex(static_cast<int>(sy), 0, height - 1)) * width
                                        + clampIndex(static_cast<int>(sx), 0, width - 1)];
                }
            }
            return result;
        }

        // Histogram of the values of a frame, one bin for each possible value
        std::vector<qint64> histogramOf(const ImageFrame& frame)
        {
            std::vector<qint64> histogram(static_cast<size_t>(maxValue(frame)) + 1, 0);
            for (int y = 0; y < frame.height; ++y)
            {
                for (int x = 0; x < frame.width; ++x)
                {
                    ++histogram[static_cast<size_t>(pixelAt(frame, x, y))];
                }
            }
            return histogram;
        }
    }

    bool isMono(const ImageFrame& frame)
    {
        return frame.isValid() && (frame.isMono8() || frame.isMono16());
    }

    int maxValue(const ImageFrame& frame)
    {
        return frame.isMono16() ? ((1 << std::clamp(frame.bitsPerSample, 1, 16)) - 1) : 255;
    }

    QRect selectionBounds(const Markup& markup)
    {
        switch (markup.type)
        {
        case MarkupType::Line:
            return QRect(markup.start, markup.end).normalized().adjusted(0, 0, 1, 1);
        case MarkupType::Point:
            return QRect(markup.start, QSize(1, 1));
        default:
            return markup.rect.normalized();
        }
    }

    double selectionPerimeter(const Markup& markup)
    {
        switch (markup.type)
        {
        case MarkupType::Line:
            return std::hypot(static_cast<double>(markup.end.x() - markup.start.x()),
                              static_cast<double>(markup.end.y() - markup.start.y()));
        case MarkupType::Point:
            return 0.0;
        case MarkupType::Rect:
        {
            const QRect shape = markup.rect.normalized();
            return 2.0 * (shape.width() + shape.height());
        }
        case MarkupType::Ellipse:
        {
            // Ramanujan's approximation
            const QRect shape = markup.rect.normalized();
            const double a = shape.width() / 2.0;
            const double b = shape.height() / 2.0;
            return 3.14159265358979323846 * (3.0 * (a + b) - std::sqrt((3.0 * a + b) * (a + 3.0 * b)));
        }
        case MarkupType::Polygon:
        {
            double length = 0.0;
            for (int index = 0; index < markup.points.size(); ++index)
            {
                const QPoint current = markup.points.at(index);
                const QPoint next = markup.points.at((index + 1) % markup.points.size());
                length += std::hypot(static_cast<double>(next.x() - current.x()),
                                     static_cast<double>(next.y() - current.y()));
            }
            return length;
        }
        }
        return 0.0;
    }

    Measurement measure(const ImageFrame& frame, const Markup& markup, double scale)
    {
        Measurement result;
        if (!isMono(frame))
        {
            return result;
        }
        const double unit = scale > 0.0 ? scale : 1.0;
        std::vector<int> values;
        double sum = 0.0;
        double sum2 = 0.0;
        double sum3 = 0.0;
        double sum4 = 0.0;
        double sumX = 0.0;
        double sumY = 0.0;
        int low = 0;
        int high = 0;
        visitPixels(frame, markup, [&](int x, int y, int value)
        {
            if (values.empty())
            {
                low = high = value;
            }
            low = std::min(low, value);
            high = std::max(high, value);
            values.push_back(value);
            const double v = value;
            sum += v;
            sum2 += v * v;
            sum3 += v * v * v;
            sum4 += v * v * v * v;
            sumX += x + 0.5;
            sumY += y + 0.5;
        });
        result.pixelCount = static_cast<qint64>(values.size());
        const QRect box = selectionBounds(markup);
        result.boundsX = box.x() * unit;
        result.boundsY = box.y() * unit;
        result.boundsWidth = box.width() * unit;
        result.boundsHeight = box.height() * unit;
        result.perimeter = selectionPerimeter(markup) * unit;
        if (values.empty())
        {
            return result;
        }
        const double n = static_cast<double>(values.size());
        result.area = n * unit * unit;
        result.mean = sum / n;
        result.stdDev = std::sqrt(std::max(0.0, sum2 / n - result.mean * result.mean));
        result.minValue = low;
        result.maxValue = high;
        result.centroidX = sumX / n * unit;
        result.centroidY = sumY / n * unit;
        result.rawIntegratedDensity = sum;
        result.integratedDensity = result.area * result.mean;
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        result.median = *middle;
        if (result.stdDev > 0.0)
        {
            const double mean = result.mean;
            const double sd = result.stdDev;
            result.skewness = ((sum3 - 3.0 * mean * sum2) / n + 2.0 * mean * mean * mean) / (sd * sd * sd);
            result.kurtosis = (((sum4 - 4.0 * mean * sum3 + 6.0 * mean * mean * sum2) / n
                                - 3.0 * mean * mean * mean * mean) / (sd * sd * sd * sd)) - 3.0;
        }
        return result;
    }

    ImageFrame blankFrame(const ImageFrame& like, int width, int height)
    {
        ImageFrame frame;
        frame.cameraId = like.cameraId;
        frame.width = width;
        frame.height = height;
        frame.pixelFormat = like.pixelFormat;
        frame.bitsPerSample = like.bitsPerSample;
        const int bytesPerPixel = like.isMono16() ? 2 : 1;
        frame.stride = width * bytesPerPixel;
        frame.frameIndex = like.frameIndex;
        frame.timestampNs = like.timestampNs;
        frame.sourceRoiX = 0;
        frame.sourceRoiY = 0;
        frame.sourceRoiWidth = width;
        frame.sourceRoiHeight = height;
        frame.bytes = QByteArray(static_cast<qsizetype>(frame.stride) * height, 0);
        return frame;
    }

    std::vector<float> toFloat(const ImageFrame& frame)
    {
        std::vector<float> values(static_cast<size_t>(frame.width) * frame.height);
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                values[static_cast<size_t>(y) * frame.width + x] = static_cast<float>(pixelAt(frame, x, y));
            }
        }
        return values;
    }

    ImageFrame fromFloat(const std::vector<float>& values, const ImageFrame& like, int width, int height)
    {
        ImageFrame frame = blankFrame(like, width, height);
        const float limit = static_cast<float>(maxValue(like));
        char* data = frame.bytes.data();
        for (int y = 0; y < height; ++y)
        {
            char* row = data + static_cast<qint64>(y) * frame.stride;
            for (int x = 0; x < width; ++x)
            {
                const float clamped = std::clamp(values[static_cast<size_t>(y) * width + x], 0.0f, limit);
                const int rounded = static_cast<int>(std::lround(clamped));
                if (frame.isMono16())
                {
                    const quint16 value = static_cast<quint16>(rounded);
                    std::memcpy(row + static_cast<qint64>(x) * 2, &value, sizeof(value));
                }
                else
                {
                    row[x] = static_cast<char>(rounded);
                }
            }
        }
        return frame;
    }

    ImageFrame zProject(const QList<ImageFrame>& frames, int first, int last, Projection method)
    {
        first = std::clamp(first, 0, static_cast<int>(frames.size()) - 1);
        last = std::clamp(last, first, static_cast<int>(frames.size()) - 1);
        const ImageFrame& like = frames.at(first);
        const size_t count = static_cast<size_t>(like.width) * like.height;
        const int slices = last - first + 1;
        std::vector<float> result(count, 0.0f);
        if (method == Projection::Max || method == Projection::Min)
        {
            result = toFloat(like);
            for (int index = first + 1; index <= last; ++index)
            {
                const std::vector<float> other = toFloat(frames.at(index));
                for (size_t i = 0; i < count; ++i)
                {
                    result[i] = method == Projection::Max ? std::max(result[i], other[i])
                                                          : std::min(result[i], other[i]);
                }
            }
        }
        else if (method == Projection::Median)
        {
            std::vector<std::vector<float>> all;
            for (int index = first; index <= last; ++index)
            {
                all.push_back(toFloat(frames.at(index)));
            }
            std::vector<float> column(static_cast<size_t>(slices));
            for (size_t i = 0; i < count; ++i)
            {
                for (int s = 0; s < slices; ++s)
                {
                    column[static_cast<size_t>(s)] = all[static_cast<size_t>(s)][i];
                }
                std::nth_element(column.begin(), column.begin() + slices / 2, column.end());
                result[i] = column[static_cast<size_t>(slices / 2)];
            }
        }
        else
        {
            std::vector<double> sum(count, 0.0);
            std::vector<double> sum2(count, 0.0);
            for (int index = first; index <= last; ++index)
            {
                const std::vector<float> other = toFloat(frames.at(index));
                for (size_t i = 0; i < count; ++i)
                {
                    sum[i] += other[i];
                    sum2[i] += static_cast<double>(other[i]) * other[i];
                }
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (method == Projection::Sum)
                {
                    result[i] = static_cast<float>(sum[i]);
                }
                else if (method == Projection::Average)
                {
                    result[i] = static_cast<float>(sum[i] / slices);
                }
                else
                {
                    const double mean = sum[i] / slices;
                    result[i] = static_cast<float>(std::sqrt(std::max(0.0, sum2[i] / slices - mean * mean)));
                }
            }
        }
        return fromFloat(result, like, like.width, like.height);
    }

    QList<ImageFrame> substack(const QList<ImageFrame>& frames, int first, int last, int step)
    {
        QList<ImageFrame> result;
        first = std::clamp(first, 0, static_cast<int>(frames.size()) - 1);
        last = std::clamp(last, first, static_cast<int>(frames.size()) - 1);
        for (int index = first; index <= last; index += std::max(1, step))
        {
            result.append(frames.at(index));
        }
        return result;
    }

    ImageFrame montage(const QList<ImageFrame>& frames, int columns, double scale, int border)
    {
        columns = std::clamp(columns, 1, static_cast<int>(frames.size()));
        const int rows = (static_cast<int>(frames.size()) + columns - 1) / columns;
        const ImageFrame& like = frames.first();
        const int tileWidth = std::max(1, static_cast<int>(std::lround(like.width * scale)));
        const int tileHeight = std::max(1, static_cast<int>(std::lround(like.height * scale)));
        const int width = columns * tileWidth + (columns + 1) * border;
        const int height = rows * tileHeight + (rows + 1) * border;
        // Frames of the other format are not mixed in
        ImageFrame result = blankFrame(like, width, height);
        std::vector<float> canvas(static_cast<size_t>(width) * height, 0.0f);
        for (int index = 0; index < frames.size(); ++index)
        {
            const ImageFrame& frame = frames.at(index);
            if (frame.pixelFormat != like.pixelFormat)
            {
                continue;
            }
            const std::vector<float> tile = resampled(toFloat(frame), frame.width, frame.height, tileWidth, tileHeight, true);
            const int originX = border + (index % columns) * (tileWidth + border);
            const int originY = border + (index / columns) * (tileHeight + border);
            for (int y = 0; y < tileHeight; ++y)
            {
                for (int x = 0; x < tileWidth; ++x)
                {
                    canvas[static_cast<size_t>(originY + y) * width + originX + x] =
                        tile[static_cast<size_t>(y) * tileWidth + x];
                }
            }
        }
        return fromFloat(canvas, like, width, height);
    }

    QVector<double> zProfile(const QList<ImageFrame>& frames, const Markup* roi, ProfileStat stat)
    {
        QVector<double> values;
        values.reserve(frames.size());
        for (const ImageFrame& frame : frames)
        {
            double sum = 0.0;
            double low = 0.0;
            double high = 0.0;
            qint64 count = 0;
            const auto add = [&](int, int, int value)
            {
                if (count == 0)
                {
                    low = high = value;
                }
                low = std::min(low, static_cast<double>(value));
                high = std::max(high, static_cast<double>(value));
                sum += value;
                ++count;
            };
            if (roi)
            {
                visitPixels(frame, *roi, add);
            }
            else
            {
                Markup whole;
                whole.type = MarkupType::Rect;
                whole.rect = QRect(0, 0, frame.width, frame.height);
                visitPixels(frame, whole, add);
            }
            double value = 0.0;
            if (count > 0)
            {
                switch (stat)
                {
                case ProfileStat::Mean: value = sum / count; break;
                case ProfileStat::Sum: value = sum; break;
                case ProfileStat::Min: value = low; break;
                case ProfileStat::Max: value = high; break;
                }
            }
            values.append(value);
        }
        return values;
    }

    ImageFrame crop(const ImageFrame& frame, const QRect& area)
    {
        const QRect inside = area.normalized().intersected(QRect(0, 0, frame.width, frame.height));
        ImageFrame result = blankFrame(frame, inside.width(), inside.height());
        const int bytesPerPixel = frame.isMono16() ? 2 : 1;
        for (int y = 0; y < inside.height(); ++y)
        {
            std::memcpy(result.bytes.data() + static_cast<qint64>(y) * result.stride,
                        frame.bytes.constData() + static_cast<qint64>(inside.y() + y) * frame.stride
                            + static_cast<qint64>(inside.x()) * bytesPerPixel,
                        static_cast<size_t>(inside.width()) * bytesPerPixel);
        }
        return result;
    }

    ImageFrame flipHorizontal(const ImageFrame& frame)
    {
        ImageFrame result = blankFrame(frame, frame.width, frame.height);
        const int bytesPerPixel = frame.isMono16() ? 2 : 1;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                std::memcpy(result.bytes.data() + static_cast<qint64>(y) * result.stride
                                + static_cast<qint64>(x) * bytesPerPixel,
                            frame.bytes.constData() + static_cast<qint64>(y) * frame.stride
                                + static_cast<qint64>(frame.width - 1 - x) * bytesPerPixel,
                            static_cast<size_t>(bytesPerPixel));
            }
        }
        return result;
    }

    ImageFrame flipVertical(const ImageFrame& frame)
    {
        ImageFrame result = blankFrame(frame, frame.width, frame.height);
        for (int y = 0; y < frame.height; ++y)
        {
            std::memcpy(result.bytes.data() + static_cast<qint64>(y) * result.stride,
                        frame.bytes.constData() + static_cast<qint64>(frame.height - 1 - y) * frame.stride,
                        static_cast<size_t>(result.stride));
        }
        return result;
    }

    ImageFrame rotate90(const ImageFrame& frame, bool clockwise)
    {
        ImageFrame result = blankFrame(frame, frame.height, frame.width);
        const int bytesPerPixel = frame.isMono16() ? 2 : 1;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const int targetX = clockwise ? frame.height - 1 - y : y;
                const int targetY = clockwise ? x : frame.width - 1 - x;
                std::memcpy(result.bytes.data() + static_cast<qint64>(targetY) * result.stride
                                + static_cast<qint64>(targetX) * bytesPerPixel,
                            frame.bytes.constData() + static_cast<qint64>(y) * frame.stride
                                + static_cast<qint64>(x) * bytesPerPixel,
                            static_cast<size_t>(bytesPerPixel));
            }
        }
        return result;
    }

    ImageFrame scaled(const ImageFrame& frame, double factorX, double factorY, bool smooth)
    {
        const int width = std::max(1, static_cast<int>(std::lround(frame.width * factorX)));
        const int height = std::max(1, static_cast<int>(std::lround(frame.height * factorY)));
        return fromFloat(resampled(toFloat(frame), frame.width, frame.height, width, height, smooth),
                         frame, width, height);
    }

    ImageFrame rotated(const ImageFrame& frame, double degrees, bool smooth, bool grow)
    {
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        int width = frame.width;
        int height = frame.height;
        if (grow)
        {
            width = static_cast<int>(std::ceil(std::abs(frame.width * cosine) + std::abs(frame.height * sine)));
            height = static_cast<int>(std::ceil(std::abs(frame.width * sine) + std::abs(frame.height * cosine)));
        }
        const std::vector<float> source = toFloat(frame);
        std::vector<float> result(static_cast<size_t>(width) * height, 0.0f);
        const double centerX = frame.width / 2.0;
        const double centerY = frame.height / 2.0;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                // Where the pixel comes from before the turn
                const double dx = x + 0.5 - width / 2.0;
                const double dy = y + 0.5 - height / 2.0;
                const double sx = centerX + dx * cosine + dy * sine;
                const double sy = centerY - dx * sine + dy * cosine;
                if (sx < 0.0 || sy < 0.0 || sx >= frame.width || sy >= frame.height)
                {
                    continue;
                }
                result[static_cast<size_t>(y) * width + x] =
                    smooth ? sampleBilinear(source, frame.width, frame.height, sx, sy)
                           : source[static_cast<size_t>(static_cast<int>(sy)) * frame.width + static_cast<int>(sx)];
            }
        }
        return fromFloat(result, frame, width, height);
    }

    ImageFrame filtered(const ImageFrame& frame, Filter filter, double radius)
    {
        const int width = frame.width;
        const int height = frame.height;
        const std::vector<float> source = toFloat(frame);
        std::vector<float> result;
        switch (filter)
        {
        case Filter::Mean:
            result = separableBlur(source, width, height, boxKernel(radius));
            break;
        case Filter::Gaussian:
            result = separableBlur(source, width, height, gaussianKernel(std::max(0.1, radius)));
            break;
        case Filter::Minimum:
            result = morphology(source, width, height, discOffsets(radius), true);
            break;
        case Filter::Maximum:
            result = morphology(source, width, height, discOffsets(radius), false);
            break;
        case Filter::Median:
        {
            const std::vector<Offset> offsets = discOffsets(radius);
            result.resize(source.size());
            std::vector<float> window(offsets.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    for (size_t k = 0; k < offsets.size(); ++k)
                    {
                        window[k] = source[static_cast<size_t>(clampIndex(y + offsets[k].dy, 0, height - 1)) * width
                                           + clampIndex(x + offsets[k].dx, 0, width - 1)];
                    }
                    std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
                    result[static_cast<size_t>(y) * width + x] = window[window.size() / 2];
                }
            }
            break;
        }
        case Filter::Variance:
        {
            const std::vector<Offset> offsets = discOffsets(radius);
            result.resize(source.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    double sum = 0.0;
                    double sum2 = 0.0;
                    for (const Offset& offset : offsets)
                    {
                        const double value = source[static_cast<size_t>(clampIndex(y + offset.dy, 0, height - 1)) * width
                                                    + clampIndex(x + offset.dx, 0, width - 1)];
                        sum += value;
                        sum2 += value * value;
                    }
                    const double n = static_cast<double>(offsets.size());
                    result[static_cast<size_t>(y) * width + x] =
                        static_cast<float>(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
                }
            }
            break;
        }
        }
        return fromFloat(result, frame, width, height);
    }

    ImageFrame unsharpMask(const ImageFrame& frame, double sigma, double weight)
    {
        const std::vector<float> source = toFloat(frame);
        const std::vector<float> blurred = separableBlur(source, frame.width, frame.height,
                                                         gaussianKernel(std::max(0.1, sigma)));
        std::vector<float> result(source.size());
        for (size_t i = 0; i < source.size(); ++i)
        {
            result[i] = static_cast<float>((source[i] - weight * blurred[i]) / (1.0 - weight));
        }
        return fromFloat(result, frame, frame.width, frame.height);
    }

    ImageFrame sharpened(const ImageFrame& frame)
    {
        const std::vector<float> source = toFloat(frame);
        std::vector<float> result(source.size());
        const int width = frame.width;
        const int height = frame.height;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                float sum = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const float weight = (dx == 0 && dy == 0) ? 12.0f : -1.0f;
                        sum += weight * source[static_cast<size_t>(clampIndex(y + dy, 0, height - 1)) * width
                                               + clampIndex(x + dx, 0, width - 1)];
                    }
                }
                result[static_cast<size_t>(y) * width + x] = sum / 4.0f;
            }
        }
        return fromFloat(result, frame, width, height);
    }

    ImageFrame findEdges(const ImageFrame& frame)
    {
        const std::vector<float> source = toFloat(frame);
        std::vector<float> result(source.size());
        const int width = frame.width;
        const int height = frame.height;
        const auto at = [&](int x, int y)
        {
            return source[static_cast<size_t>(clampIndex(y, 0, height - 1)) * width + clampIndex(x, 0, width - 1)];
        };
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const float gx = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)
                    - at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1);
                const float gy = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)
                    - at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1);
                result[static_cast<size_t>(y) * width + x] = std::sqrt(gx * gx + gy * gy);
            }
        }
        return fromFloat(result, frame, width, height);
    }

    ImageFrame subtractBackground(const ImageFrame& frame, double radius, bool lightBackground)
    {
        // The ball rolls on a smaller copy of the image, which keeps large balls affordable
        const int shrink = radius <= 10.0 ? 1 : radius <= 30.0 ? 2 : radius <= 100.0 ? 4 : 8;
        const int smallWidth = std::max(1, frame.width / shrink);
        const int smallHeight = std::max(1, frame.height / shrink);
        std::vector<float> source = toFloat(frame);
        if (lightBackground)
        {
            const float limit = static_cast<float>(maxValue(frame));
            for (float& value : source)
            {
                value = limit - value;
            }
        }
        const std::vector<float> small = resampled(source, frame.width, frame.height, smallWidth, smallHeight, true);
        const std::vector<Offset> ball = ballOffsets(std::max(1.0, radius / shrink));
        // Rolling a ball under the surface is an opening: the smallest values first, then the largest
        const std::vector<float> opened = morphology(morphology(small, smallWidth, smallHeight, ball, true),
                                                     smallWidth, smallHeight, ball, false);
        // The smoothing keeps the background from showing the blocks of the smaller image
        const std::vector<float> smooth = separableBlur(opened, smallWidth, smallHeight, boxKernel(1.0));
        const std::vector<float> background = resampled(smooth, smallWidth, smallHeight, frame.width, frame.height, true);
        std::vector<float> result(source.size());
        for (size_t i = 0; i < source.size(); ++i)
        {
            result[i] = source[i] - std::min(source[i], background[i]);
        }
        if (lightBackground)
        {
            const float limit = static_cast<float>(maxValue(frame));
            for (float& value : result)
            {
                value = limit - value;
            }
        }
        return fromFloat(result, frame, frame.width, frame.height);
    }

    ImageFrame math(const ImageFrame& frame, MathOperation operation, double value)
    {
        std::vector<float> values = toFloat(frame);
        const float limit = static_cast<float>(maxValue(frame));
        for (float& pixel : values)
        {
            switch (operation)
            {
            case MathOperation::Add: pixel += static_cast<float>(value); break;
            case MathOperation::Subtract: pixel -= static_cast<float>(value); break;
            case MathOperation::Multiply: pixel *= static_cast<float>(value); break;
            case MathOperation::Divide: pixel = value != 0.0 ? pixel / static_cast<float>(value) : limit; break;
            case MathOperation::Min: pixel = std::max(pixel, static_cast<float>(value)); break;
            case MathOperation::Max: pixel = std::min(pixel, static_cast<float>(value)); break;
            case MathOperation::Gamma:
                pixel = static_cast<float>(std::pow(pixel / limit, value) * limit);
                break;
            case MathOperation::Log:
                pixel = static_cast<float>(pixel > 0.0f ? std::log(pixel) * (limit / std::log(limit)) : 0.0);
                break;
            case MathOperation::Exp:
                pixel = static_cast<float>(std::exp(pixel * (std::log(limit) / limit)));
                break;
            case MathOperation::Square: pixel = pixel * pixel; break;
            case MathOperation::Sqrt: pixel = std::sqrt(pixel); break;
            case MathOperation::Abs: pixel = std::abs(pixel); break;
            }
        }
        return fromFloat(values, frame, frame.width, frame.height);
    }

    ImageFrame calculate(const ImageFrame& first, const ImageFrame& second, CalculatorOperation operation)
    {
        // Where the images differ in size only the part they share is used
        const int width = std::min(first.width, second.width);
        const int height = std::min(first.height, second.height);
        std::vector<float> values(static_cast<size_t>(width) * height);
        const float limit = static_cast<float>(maxValue(first));
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const float a = static_cast<float>(pixelAt(first, x, y));
                const float b = static_cast<float>(pixelAt(second, x, y));
                float r = 0.0f;
                switch (operation)
                {
                case CalculatorOperation::Add: r = a + b; break;
                case CalculatorOperation::Subtract: r = a - b; break;
                case CalculatorOperation::Multiply: r = a * b; break;
                case CalculatorOperation::Divide: r = b != 0.0f ? a / b : limit; break;
                case CalculatorOperation::And: r = static_cast<float>(static_cast<int>(a) & static_cast<int>(b)); break;
                case CalculatorOperation::Or: r = static_cast<float>(static_cast<int>(a) | static_cast<int>(b)); break;
                case CalculatorOperation::Xor: r = static_cast<float>(static_cast<int>(a) ^ static_cast<int>(b)); break;
                case CalculatorOperation::Min: r = std::min(a, b); break;
                case CalculatorOperation::Max: r = std::max(a, b); break;
                case CalculatorOperation::Average: r = (a + b) / 2.0f; break;
                case CalculatorOperation::Difference: r = std::abs(a - b); break;
                }
                values[static_cast<size_t>(y) * width + x] = r;
            }
        }
        return fromFloat(values, first, width, height);
    }

    int autoThreshold(const ImageFrame& frame, ThresholdMethod method)
    {
        const std::vector<qint64> histogram = histogramOf(frame);
        const int bins = static_cast<int>(histogram.size());
        qint64 total = 0;
        double weighted = 0.0;
        for (int i = 0; i < bins; ++i)
        {
            total += histogram[static_cast<size_t>(i)];
            weighted += static_cast<double>(i) * histogram[static_cast<size_t>(i)];
        }
        if (total == 0)
        {
            return 0;
        }
        switch (method)
        {
        case ThresholdMethod::Mean:
            return static_cast<int>(weighted / total);
        case ThresholdMethod::Otsu:
        {
            double best = -1.0;
            int threshold = 0;
            double sumBelow = 0.0;
            qint64 countBelow = 0;
            for (int t = 0; t < bins; ++t)
            {
                countBelow += histogram[static_cast<size_t>(t)];
                sumBelow += static_cast<double>(t) * histogram[static_cast<size_t>(t)];
                const qint64 countAbove = total - countBelow;
                if (countBelow == 0 || countAbove == 0)
                {
                    continue;
                }
                const double meanBelow = sumBelow / countBelow;
                const double meanAbove = (weighted - sumBelow) / countAbove;
                const double between = static_cast<double>(countBelow) * countAbove
                    * (meanBelow - meanAbove) * (meanBelow - meanAbove);
                if (between > best)
                {
                    best = between;
                    threshold = t;
                }
            }
            return threshold;
        }
        case ThresholdMethod::IsoData:
        {
            int threshold = static_cast<int>(weighted / total);
            for (int iteration = 0; iteration < 1000; ++iteration)
            {
                double sumBelow = 0.0;
                double sumAbove = 0.0;
                qint64 countBelow = 0;
                qint64 countAbove = 0;
                for (int i = 0; i < bins; ++i)
                {
                    if (i <= threshold)
                    {
                        sumBelow += static_cast<double>(i) * histogram[static_cast<size_t>(i)];
                        countBelow += histogram[static_cast<size_t>(i)];
                    }
                    else
                    {
                        sumAbove += static_cast<double>(i) * histogram[static_cast<size_t>(i)];
                        countAbove += histogram[static_cast<size_t>(i)];
                    }
                }
                if (countBelow == 0 || countAbove == 0)
                {
                    break;
                }
                const int next = static_cast<int>((sumBelow / countBelow + sumAbove / countAbove) / 2.0);
                if (next == threshold)
                {
                    break;
                }
                threshold = next;
            }
            return threshold;
        }
        case ThresholdMethod::Triangle:
        {
            int low = 0;
            int high = bins - 1;
            int peak = 0;
            for (int i = 0; i < bins; ++i)
            {
                if (histogram[static_cast<size_t>(i)] > 0)
                {
                    low = i;
                    break;
                }
            }
            for (int i = bins - 1; i >= 0; --i)
            {
                if (histogram[static_cast<size_t>(i)] > 0)
                {
                    high = i;
                    break;
                }
            }
            for (int i = 0; i < bins; ++i)
            {
                if (histogram[static_cast<size_t>(i)] > histogram[static_cast<size_t>(peak)])
                {
                    peak = i;
                }
            }
            // The line is drawn from the peak to the end that is farther away
            const bool flipped = (peak - low) < (high - peak);
            const int end = flipped ? high : low;
            const double dx = peak - end;
            const double dy = static_cast<double>(histogram[static_cast<size_t>(peak)]);
            const double norm = std::hypot(dx, dy);
            double best = -1.0;
            int threshold = peak;
            const int step = flipped ? 1 : -1;
            for (int i = peak; i != end + step * (flipped ? 1 : 1) && i >= 0 && i < bins; i += step)
            {
                const double distance = std::abs(dy * (i - end) - dx * static_cast<double>(histogram[static_cast<size_t>(i)])) / norm;
                if (distance > best)
                {
                    best = distance;
                    threshold = i;
                }
            }
            return threshold;
        }
        }
        return 0;
    }

    ImageFrame thresholded(const ImageFrame& frame, double low, double high)
    {
        ImageFrame like = frame;
        like.pixelFormat = ImagePixelFormat::Mono8;
        like.bitsPerSample = 8;
        ImageFrame result = blankFrame(like, frame.width, frame.height);
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const int value = pixelAt(frame, x, y);
                result.bytes.data()[static_cast<qint64>(y) * result.stride + x] =
                    (value >= low && value <= high) ? static_cast<char>(255) : 0;
            }
        }
        return result;
    }

    ImageFrame binary(const ImageFrame& frame, BinaryOperation operation, int iterations)
    {
        const int width = frame.width;
        const int height = frame.height;
        std::vector<unsigned char> mask(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                mask[static_cast<size_t>(y) * width + x] = pixelAt(frame, x, y) > 0 ? 1 : 0;
            }
        }
        const auto neighbours = [&](const std::vector<unsigned char>& m, int x, int y, bool wantForeground, bool eight)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if ((dx == 0 && dy == 0) || (!eight && dx != 0 && dy != 0))
                    {
                        continue;
                    }
                    const int nx = x + dx;
                    const int ny = y + dy;
                    // Outside the image counts as background
                    const bool foreground = nx >= 0 && ny >= 0 && nx < width && ny < height
                        && m[static_cast<size_t>(ny) * width + nx] != 0;
                    if (foreground == wantForeground)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        const auto erode = [&](std::vector<unsigned char>& m)
        {
            std::vector<unsigned char> next(m.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    next[static_cast<size_t>(y) * width + x] =
                        (m[static_cast<size_t>(y) * width + x] != 0 && !neighbours(m, x, y, false, true)) ? 1 : 0;
                }
            }
            m = std::move(next);
        };
        const auto dilate = [&](std::vector<unsigned char>& m)
        {
            std::vector<unsigned char> next(m.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    next[static_cast<size_t>(y) * width + x] =
                        (m[static_cast<size_t>(y) * width + x] != 0 || neighbours(m, x, y, true, true)) ? 1 : 0;
                }
            }
            m = std::move(next);
        };
        iterations = std::max(1, iterations);
        switch (operation)
        {
        case BinaryOperation::Erode:
            for (int i = 0; i < iterations; ++i) erode(mask);
            break;
        case BinaryOperation::Dilate:
            for (int i = 0; i < iterations; ++i) dilate(mask);
            break;
        case BinaryOperation::Open:
            for (int i = 0; i < iterations; ++i) erode(mask);
            for (int i = 0; i < iterations; ++i) dilate(mask);
            break;
        case BinaryOperation::Close:
            for (int i = 0; i < iterations; ++i) dilate(mask);
            for (int i = 0; i < iterations; ++i) erode(mask);
            break;
        case BinaryOperation::Invert:
            for (unsigned char& value : mask) value = value ? 0 : 1;
            break;
        case BinaryOperation::Outline:
        {
            std::vector<unsigned char> next(mask.size(), 0);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    next[static_cast<size_t>(y) * width + x] =
                        (mask[static_cast<size_t>(y) * width + x] != 0 && neighbours(mask, x, y, false, false)) ? 1 : 0;
                }
            }
            mask = std::move(next);
            break;
        }
        case BinaryOperation::FillHoles:
        {
            // Background that can be reached from the border stays, the rest is a hole
            std::vector<unsigned char> outside(mask.size(), 0);
            std::vector<int> stack;
            const auto push = [&](int x, int y)
            {
                const size_t index = static_cast<size_t>(y) * width + x;
                if (mask[index] == 0 && outside[index] == 0)
                {
                    outside[index] = 1;
                    stack.push_back(y * width + x);
                }
            };
            for (int x = 0; x < width; ++x) { push(x, 0); push(x, height - 1); }
            for (int y = 0; y < height; ++y) { push(0, y); push(width - 1, y); }
            while (!stack.empty())
            {
                const int index = stack.back();
                stack.pop_back();
                const int x = index % width;
                const int y = index / width;
                if (x > 0) push(x - 1, y);
                if (x + 1 < width) push(x + 1, y);
                if (y > 0) push(x, y - 1);
                if (y + 1 < height) push(x, y + 1);
            }
            for (size_t i = 0; i < mask.size(); ++i)
            {
                mask[i] = outside[i] ? 0 : 1;
            }
            break;
        }
        }
        ImageFrame like = frame;
        like.pixelFormat = ImagePixelFormat::Mono8;
        like.bitsPerSample = 8;
        ImageFrame result = blankFrame(like, width, height);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                result.bytes.data()[static_cast<qint64>(y) * result.stride + x] =
                    mask[static_cast<size_t>(y) * width + x] ? static_cast<char>(255) : 0;
            }
        }
        return result;
    }
}
