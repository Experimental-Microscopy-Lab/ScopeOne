#include "scopeone/RoiAnalysis.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace scopeone::core
{
    namespace
    {
        // Accumulate pixel values without storing them
        struct StatisticsAccumulator
        {
            qint64 count{0};
            double sum{0.0};
            double sumSquares{0.0};
            int minValue{0};
            int maxValue{0};

            void add(int value)
            {
                if (count == 0)
                {
                    minValue = value;
                    maxValue = value;
                }
                else
                {
                    minValue = std::min(minValue, value);
                    maxValue = std::max(maxValue, value);
                }
                ++count;
                sum += value;
                sumSquares += static_cast<double>(value) * value;
            }

            bool finish(RoiStatistics& statistics) const
            {
                statistics = RoiStatistics{};
                if (count <= 0)
                {
                    return false;
                }
                statistics.pixelCount = count;
                statistics.mean = sum / static_cast<double>(count);
                const double variance = sumSquares / static_cast<double>(count)
                    - statistics.mean * statistics.mean;
                statistics.stdDev = std::sqrt(std::max(0.0, variance));
                statistics.minValue = minValue;
                statistics.maxValue = maxValue;
                return true;
            }
        };

        int pixelValue(const ImageFrame& frame, int x, int y)
        {
            const char* row = frame.bytes.constData() + static_cast<qint64>(y) * frame.stride;
            if (frame.isMono16())
            {
                quint16 value = 0;
                std::memcpy(&value, row + static_cast<qint64>(x) * 2, sizeof(value));
                return value;
            }
            return static_cast<unsigned char>(row[x]);
        }

        bool supportedFrame(const ImageFrame& frame)
        {
            return frame.isValid() && (frame.isMono8() || frame.isMono16());
        }

        // Walk the integer pixels of a line from start to end inclusive
        template <typename Visitor>
        void visitLinePixels(const QPoint& start, const QPoint& end, Visitor&& visit)
        {
            const int dx = end.x() - start.x();
            const int dy = end.y() - start.y();
            const int steps = std::max(std::abs(dx), std::abs(dy));
            if (steps == 0)
            {
                visit(start.x(), start.y());
                return;
            }
            for (int step = 0; step <= steps; ++step)
            {
                const double t = static_cast<double>(step) / steps;
                visit(static_cast<int>(std::lround(start.x() + t * dx)),
                      static_cast<int>(std::lround(start.y() + t * dy)));
            }
        }
    }

    bool measureRoi(const ImageFrame& frame, const RoiShape& shape, RoiStatistics& statistics)
    {
        statistics = RoiStatistics{};
        if (!supportedFrame(frame))
        {
            return false;
        }

        const QRect bounds(0, 0, frame.width, frame.height);
        StatisticsAccumulator accumulator;
        switch (shape.type)
        {
        case DocumentMarkupType::Rect:
        {
            const QRect clipped = shape.rect.normalized().intersected(bounds);
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
            {
                for (int x = clipped.left(); x <= clipped.right(); ++x)
                {
                    accumulator.add(pixelValue(frame, x, y));
                }
            }
            break;
        }
        case DocumentMarkupType::Line:
            visitLinePixels(shape.start, shape.end, [&](int x, int y)
            {
                if (bounds.contains(x, y))
                {
                    accumulator.add(pixelValue(frame, x, y));
                }
            });
            break;
        }
        return accumulator.finish(statistics);
    }
} // namespace scopeone::core
