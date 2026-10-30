#include "scopeone/ImageOps.h"

#include <QSize>

#include <limits>
#include <numeric>
#include <random>

namespace scopeone::core::imageops
{

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

        // Squared distance of every foreground pixel to the nearest background pixel. The outside
        // of the image counts as background.
        std::vector<float> squaredDistances(const std::vector<unsigned char>& mask, int width, int height)
        {
            const int w = width + 2;
            const int h = height + 2;
            const float infinity = 1.0e12f;
            std::vector<float> f(static_cast<size_t>(w) * h, 0.0f);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    f[static_cast<size_t>(y + 1) * w + x + 1] = mask[static_cast<size_t>(y) * width + x] ? infinity : 0.0f;
                }
            }
            // One dimensional transform of Felzenszwalb and Huttenlocher, on rows and then columns
            const auto transform = [&](std::vector<float>& line)
            {
                const int n = static_cast<int>(line.size());
                std::vector<int> v(static_cast<size_t>(n));
                std::vector<float> z(static_cast<size_t>(n) + 1);
                std::vector<float> d(static_cast<size_t>(n));
                int k = 0;
                v[0] = 0;
                z[0] = -infinity;
                z[1] = infinity;
                for (int q = 1; q < n; ++q)
                {
                    float s = 0.0f;
                    while (true)
                    {
                        const int p = v[static_cast<size_t>(k)];
                        s = ((line[static_cast<size_t>(q)] + static_cast<float>(q) * q)
                             - (line[static_cast<size_t>(p)] + static_cast<float>(p) * p)) / (2.0f * (q - p));
                        if (s <= z[static_cast<size_t>(k)] && k > 0)
                        {
                            --k;
                        }
                        else
                        {
                            break;
                        }
                    }
                    ++k;
                    v[static_cast<size_t>(k)] = q;
                    z[static_cast<size_t>(k)] = s;
                    z[static_cast<size_t>(k) + 1] = infinity;
                }
                k = 0;
                for (int q = 0; q < n; ++q)
                {
                    while (z[static_cast<size_t>(k) + 1] < q)
                    {
                        ++k;
                    }
                    const int p = v[static_cast<size_t>(k)];
                    d[static_cast<size_t>(q)] = static_cast<float>(q - p) * (q - p) + line[static_cast<size_t>(p)];
                }
                line = d;
            };
            std::vector<float> line;
            for (int x = 0; x < w; ++x)
            {
                line.assign(static_cast<size_t>(h), 0.0f);
                for (int y = 0; y < h; ++y) line[static_cast<size_t>(y)] = f[static_cast<size_t>(y) * w + x];
                transform(line);
                for (int y = 0; y < h; ++y) f[static_cast<size_t>(y) * w + x] = line[static_cast<size_t>(y)];
            }
            for (int y = 0; y < h; ++y)
            {
                line.assign(f.begin() + static_cast<std::ptrdiff_t>(y) * w, f.begin() + static_cast<std::ptrdiff_t>(y + 1) * w);
                transform(line);
                std::copy(line.begin(), line.end(), f.begin() + static_cast<std::ptrdiff_t>(y) * w);
            }
            std::vector<float> result(static_cast<size_t>(width) * height);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    result[static_cast<size_t>(y) * width + x] = f[static_cast<size_t>(y + 1) * w + x + 1];
                }
            }
            return result;
        }

        std::vector<unsigned char> maskOf(const ImageFrame& frame)
        {
            std::vector<unsigned char> mask(static_cast<size_t>(frame.width) * frame.height);
            for (int y = 0; y < frame.height; ++y)
            {
                for (int x = 0; x < frame.width; ++x)
                {
                    mask[static_cast<size_t>(y) * frame.width + x] = pixelAt(frame, x, y) > 0 ? 1 : 0;
                }
            }
            return mask;
        }

        QRect convexBounds(const QList<QPoint>& points)
        {
            int left = points.first().x();
            int right = left;
            int top = points.first().y();
            int bottom = top;
            for (const QPoint& point : points)
            {
                left = std::min(left, point.x());
                right = std::max(right, point.x());
                top = std::min(top, point.y());
                bottom = std::max(bottom, point.y());
            }
            return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
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
        return autoThreshold(histogramOf(frame), method);
    }

    int autoThreshold(const std::vector<qint64>& histogram, ThresholdMethod method)
    {
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

    QList<Particle> findParticles(const ImageFrame& mask)
    {
        const int width = mask.width;
        const int height = mask.height;
        const std::vector<unsigned char> foreground = maskOf(mask);
        std::vector<int> label(foreground.size(), 0);
        QList<Particle> particles;
        const auto inside = [&](int x, int y, int wanted)
        {
            return x >= 0 && y >= 0 && x < width && y < height && label[static_cast<size_t>(y) * width + x] == wanted;
        };
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t start = static_cast<size_t>(y) * width + x;
                if (!foreground[start] || label[start] != 0)
                {
                    continue;
                }
                const int wanted = static_cast<int>(particles.size()) + 1;
                Particle particle;
                int left = x, right = x, top = y, bottom = y;
                std::vector<int> stack{static_cast<int>(start)};
                label[start] = wanted;
                while (!stack.empty())
                {
                    const int index = stack.back();
                    stack.pop_back();
                    particle.pixels.push_back(index);
                    const int px = index % width;
                    const int py = index / width;
                    left = std::min(left, px);
                    right = std::max(right, px);
                    top = std::min(top, py);
                    bottom = std::max(bottom, py);
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = px + dx;
                            const int ny = py + dy;
                            if (nx >= 0 && ny >= 0 && nx < width && ny < height
                                && foreground[static_cast<size_t>(ny) * width + nx]
                                && label[static_cast<size_t>(ny) * width + nx] == 0)
                            {
                                label[static_cast<size_t>(ny) * width + nx] = wanted;
                                stack.push_back(ny * width + nx);
                            }
                        }
                    }
                }
                particle.bounds = QRect(left, top, right - left + 1, bottom - top + 1);
                particle.touchesEdge = left == 0 || top == 0 || right == width - 1 || bottom == height - 1;

                // Follows the outer edge of the pixels with the particle on the right hand
                static const int stepX[4] = {1, 0, -1, 0};
                static const int stepY[4] = {0, 1, 0, -1};
                int cornerX = x;
                int cornerY = y;
                int direction = 0;
                int edges = 0;
                int turns = 0;
                do
                {
                    cornerX += stepX[direction];
                    cornerY += stepY[direction];
                    ++edges;
                    bool aheadLeft = false;
                    bool aheadRight = false;
                    switch (direction)
                    {
                    case 0: aheadLeft = inside(cornerX, cornerY - 1, wanted); aheadRight = inside(cornerX, cornerY, wanted); break;
                    case 1: aheadLeft = inside(cornerX, cornerY, wanted); aheadRight = inside(cornerX - 1, cornerY, wanted); break;
                    case 2: aheadLeft = inside(cornerX - 1, cornerY, wanted); aheadRight = inside(cornerX - 1, cornerY - 1, wanted); break;
                    default: aheadLeft = inside(cornerX - 1, cornerY - 1, wanted); aheadRight = inside(cornerX, cornerY - 1, wanted); break;
                    }
                    const int next = aheadLeft ? (direction + 3) % 4 : (aheadRight ? direction : (direction + 1) % 4);
                    if (next != direction)
                    {
                        particle.outline.append(QPoint(cornerX, cornerY));
                        ++turns;
                    }
                    direction = next;
                } while (!(cornerX == x && cornerY == y && direction == 0));
                particle.perimeter = edges - turns * (2.0 - std::sqrt(2.0));
                particles.append(std::move(particle));
            }
        }
        return particles;
    }

    Measurement measureParticle(const ImageFrame& image, const Particle& particle, double scale, bool includeHoles)
    {
        const double unit = scale > 0.0 ? scale : 1.0;
        Measurement result;
        if (includeHoles)
        {
            Markup outline;
            outline.type = MarkupType::Polygon;
            outline.points = particle.outline;
            outline.rect = particle.bounds;
            result = measure(image, outline, scale);
        }
        else
        {
            std::vector<int> values;
            double sum = 0.0;
            double sum2 = 0.0;
            double sumX = 0.0;
            double sumY = 0.0;
            for (const int index : particle.pixels)
            {
                const int x = index % image.width;
                const int y = index / image.width;
                const int value = pixelAt(image, x, y);
                values.push_back(value);
                sum += value;
                sum2 += static_cast<double>(value) * value;
                sumX += x + 0.5;
                sumY += y + 0.5;
            }
            const double n = static_cast<double>(values.size());
            result.pixelCount = static_cast<qint64>(values.size());
            result.area = n * unit * unit;
            result.mean = sum / n;
            result.stdDev = std::sqrt(std::max(0.0, sum2 / n - result.mean * result.mean));
            result.minValue = *std::min_element(values.begin(), values.end());
            result.maxValue = *std::max_element(values.begin(), values.end());
            const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), middle, values.end());
            result.median = *middle;
            result.centroidX = sumX / n * unit;
            result.centroidY = sumY / n * unit;
            result.rawIntegratedDensity = sum;
            result.integratedDensity = result.area * result.mean;
        }
        result.boundsX = particle.bounds.x() * unit;
        result.boundsY = particle.bounds.y() * unit;
        result.boundsWidth = particle.bounds.width() * unit;
        result.boundsHeight = particle.bounds.height() * unit;
        result.perimeter = particle.perimeter * unit;

        // The ellipse that has the same second moments as the particle
        double meanX = 0.0;
        double meanY = 0.0;
        const double count = static_cast<double>(particle.pixels.size());
        for (const int index : particle.pixels)
        {
            meanX += index % image.width + 0.5;
            meanY += index / image.width + 0.5;
        }
        meanX /= count;
        meanY /= count;
        double xx = 1.0 / 12.0;
        double yy = 1.0 / 12.0;
        double xy = 0.0;
        for (const int index : particle.pixels)
        {
            const double dx = index % image.width + 0.5 - meanX;
            const double dy = index / image.width + 0.5 - meanY;
            xx += dx * dx / count;
            yy += dy * dy / count;
            xy += dx * dy / count;
        }
        const double root = std::sqrt((xx - yy) * (xx - yy) + 4.0 * xy * xy);
        const double major = 4.0 * std::sqrt(std::max(0.0, (xx + yy + root) / 2.0));
        const double minor = 4.0 * std::sqrt(std::max(0.0, (xx + yy - root) / 2.0));
        result.majorAxis = major * unit;
        result.minorAxis = minor * unit;
        result.aspectRatio = minor > 0.0 ? major / minor : 0.0;
        result.roundness = major > 0.0 ? std::min(1.0, 4.0 * count / (3.14159265358979323846 * major * major)) : 0.0;
        result.circularity = particle.perimeter > 0.0
            ? std::min(1.0, 4.0 * 3.14159265358979323846 * count / (particle.perimeter * particle.perimeter))
            : 0.0;

        // The convex hull of the outline gives the solidity and the longest distance across
        QList<QPoint> points = particle.outline;
        std::sort(points.begin(), points.end(), [](const QPoint& a, const QPoint& b)
        {
            return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
        });
        points.erase(std::unique(points.begin(), points.end()), points.end());
        const auto cross = [](const QPoint& o, const QPoint& a, const QPoint& b)
        {
            return static_cast<qint64>(a.x() - o.x()) * (b.y() - o.y()) - static_cast<qint64>(a.y() - o.y()) * (b.x() - o.x());
        };
        QList<QPoint> hull;
        if (points.size() >= 3)
        {
            hull.resize(points.size() * 2);
            int k = 0;
            for (int i = 0; i < points.size(); ++i)
            {
                while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
                hull[k++] = points[i];
            }
            for (int i = static_cast<int>(points.size()) - 2, t = k + 1; i >= 0; --i)
            {
                while (k >= t && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
                hull[k++] = points[i];
            }
            hull.resize(k - 1);
        }
        else
        {
            hull = points;
        }
        double hullArea = 0.0;
        double longest = 0.0;
        for (int i = 0; i < hull.size(); ++i)
        {
            const QPoint a = hull.at(i);
            const QPoint b = hull.at((i + 1) % hull.size());
            hullArea += static_cast<double>(a.x()) * b.y() - static_cast<double>(b.x()) * a.y();
            for (int j = i + 1; j < hull.size(); ++j)
            {
                longest = std::max(longest, std::hypot(static_cast<double>(a.x() - hull.at(j).x()),
                                                       static_cast<double>(a.y() - hull.at(j).y())));
            }
        }
        hullArea = std::abs(hullArea) / 2.0;
        result.solidity = hullArea > 0.0 ? std::min(1.0, count / hullArea) : 0.0;
        result.feret = longest * unit;
        return result;
    }

    ImageFrame particleImage(const QList<Particle>& particles, int width, int height, bool numbered)
    {
        ImageFrame like;
        like.pixelFormat = numbered ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
        like.bitsPerSample = numbered ? 16 : 8;
        ImageFrame result = blankFrame(like, width, height);
        for (int number = 0; number < particles.size(); ++number)
        {
            for (const int index : particles.at(number).pixels)
            {
                const int x = index % width;
                const int y = index / width;
                if (numbered)
                {
                    const quint16 value = static_cast<quint16>(std::min(number + 1, 65535));
                    std::memcpy(result.bytes.data() + static_cast<qint64>(y) * result.stride + static_cast<qint64>(x) * 2,
                                &value, sizeof(value));
                }
                else
                {
                    result.bytes.data()[static_cast<qint64>(y) * result.stride + x] = static_cast<char>(255);
                }
            }
        }
        return result;
    }

    ImageFrame selectionMask(int width, int height, const Markup& selection)
    {
        ImageFrame like;
        like.pixelFormat = ImagePixelFormat::Mono8;
        like.bitsPerSample = 8;
        ImageFrame canvas = blankFrame(like, width, height);
        ImageFrame result = blankFrame(like, width, height);
        visitPixels(canvas, selection, [&](int x, int y, int)
        {
            result.bytes.data()[static_cast<qint64>(y) * result.stride + x] = static_cast<char>(255);
        });
        return result;
    }

    QList<QPoint> convexHull(const QList<QPoint>& input)
    {
        QList<QPoint> points = input;
        std::sort(points.begin(), points.end(), [](const QPoint& a, const QPoint& b)
        {
            return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
        });
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (points.size() < 3)
        {
            return points;
        }
        const auto cross = [](const QPoint& o, const QPoint& a, const QPoint& b)
        {
            return static_cast<qint64>(a.x() - o.x()) * (b.y() - o.y()) - static_cast<qint64>(a.y() - o.y()) * (b.x() - o.x());
        };
        QList<QPoint> hull(points.size() * 2);
        int k = 0;
        for (int i = 0; i < points.size(); ++i)
        {
            while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
            hull[k++] = points[i];
        }
        for (int i = static_cast<int>(points.size()) - 2, t = k + 1; i >= 0; --i)
        {
            while (k >= t && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
            hull[k++] = points[i];
        }
        hull.resize(k - 1);
        return hull;
    }

    QList<QPoint> selectionCorners(const Markup& selection)
    {
        switch (selection.type)
        {
        case MarkupType::Polygon:
            return selection.points;
        case MarkupType::Line:
            return {selection.start, selection.end};
        case MarkupType::Point:
            return {selection.start};
        case MarkupType::Rect:
        {
            const QRect shape = selection.rect.normalized();
            return {shape.topLeft(), QPoint(shape.right() + 1, shape.top()),
                    QPoint(shape.right() + 1, shape.bottom() + 1), QPoint(shape.left(), shape.bottom() + 1)};
        }
        case MarkupType::Ellipse:
        {
            const QRect shape = selection.rect.normalized();
            const double radiusX = shape.width() / 2.0;
            const double radiusY = shape.height() / 2.0;
            QList<QPoint> ring;
            for (int step = 0; step < 72; ++step)
            {
                const double angle = step * 3.14159265358979323846 / 36.0;
                ring.append(QPoint(static_cast<int>(std::lround(shape.x() + radiusX + radiusX * std::cos(angle))),
                                   static_cast<int>(std::lround(shape.y() + radiusY + radiusY * std::sin(angle)))));
            }
            return ring;
        }
        }
        return {};
    }

    QList<QPoint> simplifiedOutline(const QList<QPoint>& points, double epsilon)
    {
        if (points.size() < 4)
        {
            return points;
        }
        // Douglas and Peucker, on the closed outline cut at its first and its farthest point
        const auto distance = [](const QPoint& p, const QPoint& a, const QPoint& b)
        {
            const double dx = b.x() - a.x();
            const double dy = b.y() - a.y();
            const double length = std::hypot(dx, dy);
            if (length == 0.0)
            {
                return std::hypot(static_cast<double>(p.x() - a.x()), static_cast<double>(p.y() - a.y()));
            }
            return std::abs(dy * (p.x() - a.x()) - dx * (p.y() - a.y())) / length;
        };
        std::vector<char> keep(static_cast<size_t>(points.size()), 0);
        int far = 0;
        double farthest = -1.0;
        for (int i = 1; i < points.size(); ++i)
        {
            const double d = std::hypot(static_cast<double>(points[i].x() - points[0].x()),
                                        static_cast<double>(points[i].y() - points[0].y()));
            if (d > farthest)
            {
                farthest = d;
                far = i;
            }
        }
        keep[0] = 1;
        keep[static_cast<size_t>(far)] = 1;
        struct Span { int first; int last; };
        std::vector<Span> stack{{0, far}, {far, static_cast<int>(points.size())}};
        while (!stack.empty())
        {
            const Span span = stack.back();
            stack.pop_back();
            const QPoint& a = points[span.first];
            const QPoint& b = points[span.last % points.size()];
            int index = -1;
            double worst = epsilon;
            for (int i = span.first + 1; i < span.last; ++i)
            {
                const double d = distance(points[i], a, b);
                if (d > worst)
                {
                    worst = d;
                    index = i;
                }
            }
            if (index >= 0)
            {
                keep[static_cast<size_t>(index)] = 1;
                stack.push_back({span.first, index});
                stack.push_back({index, span.last});
            }
        }
        QList<QPoint> result;
        for (int i = 0; i < points.size(); ++i)
        {
            if (keep[static_cast<size_t>(i)])
            {
                result.append(points[i]);
            }
        }
        return result.size() >= 3 ? result : points;
    }

    QList<QPoint> enlargedOutline(int width, int height, const Markup& selection, double pixels)
    {
        // Pad the image, so that a selection at the edge can grow beyond it
        const int pad = pixels > 0.0 ? static_cast<int>(std::ceil(pixels)) + 1 : 1;
        const int paddedWidth = width + 2 * pad;
        const int paddedHeight = height + 2 * pad;
        std::vector<unsigned char> inside(static_cast<size_t>(paddedWidth) * paddedHeight, 0);
        const ImageFrame mask = selectionMask(width, height, selection);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                inside[static_cast<size_t>(y + pad) * paddedWidth + x + pad] = pixelAt(mask, x, y) > 0 ? 1 : 0;
            }
        }
        std::vector<unsigned char> grown(inside.size(), 0);
        if (pixels >= 0.0)
        {
            // Everything within the distance of the selection
            std::vector<unsigned char> outside(inside.size());
            for (size_t i = 0; i < inside.size(); ++i)
            {
                outside[i] = inside[i] ? 0 : 1;
            }
            const std::vector<float> distance = squaredDistances(outside, paddedWidth, paddedHeight);
            for (size_t i = 0; i < inside.size(); ++i)
            {
                grown[i] = (inside[i] || distance[i] <= pixels * pixels) ? 1 : 0;
            }
        }
        else
        {
            const std::vector<float> distance = squaredDistances(inside, paddedWidth, paddedHeight);
            for (size_t i = 0; i < inside.size(); ++i)
            {
                grown[i] = (inside[i] && distance[i] > pixels * pixels) ? 1 : 0;
            }
        }
        ImageFrame like;
        like.pixelFormat = ImagePixelFormat::Mono8;
        like.bitsPerSample = 8;
        ImageFrame grownFrame = blankFrame(like, paddedWidth, paddedHeight);
        for (int y = 0; y < paddedHeight; ++y)
        {
            for (int x = 0; x < paddedWidth; ++x)
            {
                grownFrame.bytes.data()[static_cast<qint64>(y) * grownFrame.stride + x] =
                    grown[static_cast<size_t>(y) * paddedWidth + x] ? static_cast<char>(255) : 0;
            }
        }
        // The largest particle is the new selection
        const QList<Particle> particles = findParticles(grownFrame);
        const Particle* largest = nullptr;
        for (const Particle& particle : particles)
        {
            if (!largest || particle.pixels.size() > largest->pixels.size())
            {
                largest = &particle;
            }
        }
        if (!largest)
        {
            return {};
        }
        QList<QPoint> outline;
        for (const QPoint& corner : largest->outline)
        {
            outline.append(QPoint(std::clamp(corner.x() - pad, 0, width), std::clamp(corner.y() - pad, 0, height)));
        }
        return outline;
    }

    QList<QPoint> fittedEllipse(int width, int height, const Markup& selection)
    {
        const ImageFrame mask = selectionMask(width, height, selection);
        double count = 0.0;
        double meanX = 0.0;
        double meanY = 0.0;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (pixelAt(mask, x, y) > 0)
                {
                    count += 1.0;
                    meanX += x + 0.5;
                    meanY += y + 0.5;
                }
            }
        }
        if (count < 3.0)
        {
            return {};
        }
        meanX /= count;
        meanY /= count;
        double xx = 1.0 / 12.0;
        double yy = 1.0 / 12.0;
        double xy = 0.0;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (pixelAt(mask, x, y) > 0)
                {
                    const double dx = x + 0.5 - meanX;
                    const double dy = y + 0.5 - meanY;
                    xx += dx * dx / count;
                    yy += dy * dy / count;
                    xy += dx * dy / count;
                }
            }
        }
        const double root = std::sqrt((xx - yy) * (xx - yy) + 4.0 * xy * xy);
        const double major = 2.0 * std::sqrt(std::max(0.0, (xx + yy + root) / 2.0));
        const double minor = 2.0 * std::sqrt(std::max(0.0, (xx + yy - root) / 2.0));
        const double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
        QList<QPoint> ring;
        for (int step = 0; step < 72; ++step)
        {
            const double t = step * 3.14159265358979323846 / 36.0;
            const double ex = major * std::cos(t);
            const double ey = minor * std::sin(t);
            const QPoint point(static_cast<int>(std::lround(meanX + ex * std::cos(angle) - ey * std::sin(angle))),
                               static_cast<int>(std::lround(meanY + ex * std::sin(angle) + ey * std::cos(angle))));
            if (ring.isEmpty() || ring.last() != point)
            {
                ring.append(point);
            }
        }
        return ring;
    }

    QList<QPoint> rotatedCorners(const Markup& selection, double degrees)
    {
        const QList<QPoint> corners = selectionCorners(selection);
        if (corners.isEmpty())
        {
            return {};
        }
        double centerX = 0.0;
        double centerY = 0.0;
        const QRect box = selection.type == MarkupType::Line || selection.type == MarkupType::Point
            ? selectionBounds(selection) : QRect(convexBounds(corners));
        centerX = box.x() + box.width() / 2.0;
        centerY = box.y() + box.height() / 2.0;
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        QList<QPoint> result;
        for (const QPoint& corner : corners)
        {
            const double dx = corner.x() - centerX;
            const double dy = corner.y() - centerY;
            result.append(QPoint(static_cast<int>(std::lround(centerX + dx * std::cos(radians) - dy * std::sin(radians))),
                                 static_cast<int>(std::lround(centerY + dx * std::sin(radians) + dy * std::cos(radians)))));
        }
        return result;
    }

    QList<QPoint> scaledCorners(const Markup& selection, double factorX, double factorY)
    {
        const QList<QPoint> corners = selectionCorners(selection);
        if (corners.isEmpty())
        {
            return {};
        }
        const QRect box = QRect(convexBounds(corners));
        const double centerX = box.x() + box.width() / 2.0;
        const double centerY = box.y() + box.height() / 2.0;
        QList<QPoint> result;
        for (const QPoint& corner : corners)
        {
            result.append(QPoint(static_cast<int>(std::lround(centerX + (corner.x() - centerX) * factorX)),
                                 static_cast<int>(std::lround(centerY + (corner.y() - centerY) * factorY))));
        }
        return result;
    }

    QList<QPoint> wandOutline(const ImageFrame& frame, const QPoint& seed, int tolerance)
    {
        if (!isMono(frame) || !QRect(0, 0, frame.width, frame.height).contains(seed))
        {
            return {};
        }
        const int reference = pixelAt(frame, seed.x(), seed.y());
        ImageFrame like;
        like.pixelFormat = ImagePixelFormat::Mono8;
        like.bitsPerSample = 8;
        ImageFrame mask = blankFrame(like, frame.width, frame.height);
        std::vector<int> stack{seed.y() * frame.width + seed.x()};
        std::vector<char> seen(static_cast<size_t>(frame.width) * frame.height, 0);
        seen[static_cast<size_t>(stack.back())] = 1;
        while (!stack.empty())
        {
            const int index = stack.back();
            stack.pop_back();
            const int x = index % frame.width;
            const int y = index / frame.width;
            mask.bytes.data()[static_cast<qint64>(y) * mask.stride + x] = static_cast<char>(255);
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= frame.width || ny >= frame.height)
                    {
                        continue;
                    }
                    const size_t next = static_cast<size_t>(ny) * frame.width + nx;
                    if (!seen[next] && std::abs(pixelAt(frame, nx, ny) - reference) <= tolerance)
                    {
                        seen[next] = 1;
                        stack.push_back(static_cast<int>(next));
                    }
                }
            }
        }
        const QList<Particle> particles = findParticles(mask);
        return particles.isEmpty() ? QList<QPoint>{} : particles.first().outline;
    }

    std::vector<qint64> histogramOfFrames(const QList<ImageFrame>& frames)
    {
        std::vector<qint64> total;
        for (const ImageFrame& frame : frames)
        {
            const std::vector<qint64> histogram = histogramOf(frame);
            if (total.size() < histogram.size())
            {
                total.resize(histogram.size(), 0);
            }
            for (size_t i = 0; i < histogram.size(); ++i)
            {
                total[i] += histogram[i];
            }
        }
        return total;
    }

    LevelRange saturatedRange(const std::vector<qint64>& histogram, double saturatedPercent)
    {
        qint64 total = 0;
        for (const qint64 count : histogram)
        {
            total += count;
        }
        const qint64 threshold = saturatedPercent > 0.0 ? static_cast<qint64>(total * saturatedPercent / 200.0) : 0;
        LevelRange range;
        qint64 count = 0;
        int low = -1;
        do
        {
            ++low;
            count += histogram[static_cast<size_t>(low)];
        } while (count <= threshold && low < static_cast<int>(histogram.size()) - 1);
        count = 0;
        int high = static_cast<int>(histogram.size());
        do
        {
            --high;
            count += histogram[static_cast<size_t>(high)];
        } while (count <= threshold && high > 0);
        range.low = low;
        range.high = std::max(low, high);
        return range;
    }

    ImageFrame stretched(const ImageFrame& frame, int low, int high)
    {
        std::vector<float> values = toFloat(frame);
        const float limit = static_cast<float>(maxValue(frame));
        const float span = static_cast<float>(std::max(1, high - low));
        for (float& value : values)
        {
            value = (value - static_cast<float>(low)) / span * limit;
        }
        return fromFloat(values, frame, frame.width, frame.height);
    }

    ImageFrame equalized(const ImageFrame& frame, bool sqrtCounts, const std::vector<qint64>* histogram)
    {
        const std::vector<float> lookup = equalizationTable(histogram ? *histogram : histogramOf(frame), sqrtCounts);
        const int top = static_cast<int>(lookup.size()) - 1;
        std::vector<float> values = toFloat(frame);
        for (float& value : values)
        {
            value = lookup[static_cast<size_t>(std::clamp(static_cast<int>(value), 0, top))];
        }
        return fromFloat(values, frame, frame.width, frame.height);
    }

    std::vector<float> equalizationTable(const std::vector<qint64>& counts, bool sqrtCounts)
    {
        const int top = static_cast<int>(counts.size()) - 1;
        std::vector<double> weight(counts.size());
        for (size_t i = 0; i < counts.size(); ++i)
        {
            weight[i] = sqrtCounts ? std::sqrt(static_cast<double>(counts[i])) : static_cast<double>(counts[i]);
        }
        // Every value counts twice but the first and the last, as in ImageJ
        double sum = weight[0] + weight[static_cast<size_t>(top)];
        for (int i = 1; i < top; ++i)
        {
            sum += 2.0 * weight[static_cast<size_t>(i)];
        }
        const double scale = sum > 0.0 ? top / sum : 1.0;
        std::vector<float> lookup(counts.size());
        double running = weight[0];
        lookup[0] = 0.0f;
        for (int i = 1; i < top; ++i)
        {
            const double delta = weight[static_cast<size_t>(i)];
            running += delta;
            lookup[static_cast<size_t>(i)] = static_cast<float>(std::round(running * scale));
            running += delta;
        }
        lookup[static_cast<size_t>(top)] = static_cast<float>(top);
        return lookup;
    }

    ImageFrame withGaussianNoise(const ImageFrame& frame, double sigma)
    {
        std::vector<float> values = toFloat(frame);
        std::mt19937 generator(std::random_device{}());
        std::normal_distribution<float> noise(0.0f, static_cast<float>(sigma));
        for (float& value : values)
        {
            value += noise(generator);
        }
        return fromFloat(values, frame, frame.width, frame.height);
    }

    ImageFrame withSaltAndPepper(const ImageFrame& frame, double percent)
    {
        std::vector<float> values = toFloat(frame);
        std::mt19937 generator(std::random_device{}());
        std::uniform_real_distribution<double> chance(0.0, 100.0);
        const float limit = static_cast<float>(maxValue(frame));
        for (float& value : values)
        {
            const double roll = chance(generator);
            if (roll < percent / 2.0)
            {
                value = 0.0f;
            }
            else if (roll < percent)
            {
                value = limit;
            }
        }
        return fromFloat(values, frame, frame.width, frame.height);
    }

}
