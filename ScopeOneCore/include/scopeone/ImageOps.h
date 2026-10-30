#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/ImageSceneModel.h"
#include "scopeone/scopeone_core_export.h"

#include <QList>
#include <QPoint>
#include <QPolygonF>
#include <QRect>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// Operations on the images of ScopeOne in the manner of ImageJ: measurements of selections,
// stack tools, transforms, filters, math and binary operations. They work on mono 8 bit and
// 16 bit frames and make new frames, so the images they start from stay as they are.
namespace scopeone::core::imageops
{
    using Markup = ImageSceneModel::Markup;
    using MarkupType = DocumentMarkupType;

    SCOPEONE_CORE_EXPORT bool isMono(const ImageFrame& frame);
    SCOPEONE_CORE_EXPORT int maxValue(const ImageFrame& frame);

    inline int pixelAt(const ImageFrame& frame, int x, int y)
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

    // ---- selections

    // Calls visit(x, y, value) for every pixel of the frame that a selection covers. A pixel
    // belongs to a rectangle, ellipse or polygon when its center is inside. A line covers the
    // pixels on it and a point its own pixel.
    template <typename Visitor>
    void visitPixels(const ImageFrame& frame, const Markup& markup, Visitor&& visit)
    {
        const QRect bounds(0, 0, frame.width, frame.height);
        switch (markup.type)
        {
        case MarkupType::Line:
        {
            const QPoint delta = markup.end - markup.start;
            const int steps = std::max(std::abs(delta.x()), std::abs(delta.y()));
            for (int step = 0; step <= steps; ++step)
            {
                const double t = steps == 0 ? 0.0 : static_cast<double>(step) / steps;
                const int x = static_cast<int>(std::lround(markup.start.x() + t * delta.x()));
                const int y = static_cast<int>(std::lround(markup.start.y() + t * delta.y()));
                if (bounds.contains(x, y))
                {
                    visit(x, y, pixelAt(frame, x, y));
                }
            }
            break;
        }
        case MarkupType::Point:
            if (bounds.contains(markup.start))
            {
                visit(markup.start.x(), markup.start.y(), pixelAt(frame, markup.start.x(), markup.start.y()));
            }
            break;
        case MarkupType::Ellipse:
        {
            const QRect shape = markup.rect.normalized();
            const double radiusX = shape.width() / 2.0;
            const double radiusY = shape.height() / 2.0;
            const double centerX = shape.x() + radiusX;
            const double centerY = shape.y() + radiusY;
            const QRect area = shape.intersected(bounds);
            for (int y = area.top(); y <= area.bottom(); ++y)
            {
                for (int x = area.left(); x <= area.right(); ++x)
                {
                    const double dx = (x + 0.5 - centerX) / radiusX;
                    const double dy = (y + 0.5 - centerY) / radiusY;
                    if (dx * dx + dy * dy <= 1.0)
                    {
                        visit(x, y, pixelAt(frame, x, y));
                    }
                }
            }
            break;
        }
        case MarkupType::Polygon:
        {
            QPolygonF polygon;
            for (const QPoint& corner : markup.points)
            {
                polygon << QPointF(corner);
            }
            const QRect area = polygon.boundingRect().toAlignedRect().intersected(bounds);
            for (int y = area.top(); y <= area.bottom(); ++y)
            {
                for (int x = area.left(); x <= area.right(); ++x)
                {
                    if (polygon.containsPoint(QPointF(x + 0.5, y + 0.5), Qt::OddEvenFill))
                    {
                        visit(x, y, pixelAt(frame, x, y));
                    }
                }
            }
            break;
        }
        case MarkupType::Rect:
        {
            const QRect area = markup.rect.normalized().intersected(bounds);
            for (int y = area.top(); y <= area.bottom(); ++y)
            {
                for (int x = area.left(); x <= area.right(); ++x)
                {
                    visit(x, y, pixelAt(frame, x, y));
                }
            }
            break;
        }
        }
    }

    // The smallest rectangle that holds the selection, in pixels
    SCOPEONE_CORE_EXPORT QRect selectionBounds(const Markup& markup);
    // Length of the outline, or of the line, in pixels
    SCOPEONE_CORE_EXPORT double selectionPerimeter(const Markup& markup);

    // What ImageJ measures of a selection. Lengths and areas follow scale, the size of a pixel
    // in the unit of the image, and are in pixels when it is zero.
    struct Measurement
    {
        qint64 pixelCount{0};
        double area{0.0};
        double mean{0.0};
        double stdDev{0.0};
        double median{0.0};
        double minValue{0.0};
        double maxValue{0.0};
        double centroidX{0.0};
        double centroidY{0.0};
        double perimeter{0.0};
        double boundsX{0.0};
        double boundsY{0.0};
        double boundsWidth{0.0};
        double boundsHeight{0.0};
        double integratedDensity{0.0};
        double rawIntegratedDensity{0.0};
        double skewness{0.0};
        double kurtosis{0.0};
        // Shape of a particle, which only particles have
        double circularity{0.0};
        double aspectRatio{0.0};
        double roundness{0.0};
        double solidity{0.0};
        double feret{0.0};
        double majorAxis{0.0};
        double minorAxis{0.0};
    };
    SCOPEONE_CORE_EXPORT Measurement measure(const ImageFrame& frame, const Markup& markup, double scale);

    // ---- changing selections

    // The pixels of the selection as an 8 bit frame of the given size, 255 inside
    SCOPEONE_CORE_EXPORT ImageFrame selectionMask(int width, int height, const Markup& selection);
    // Corners of the outline as a polygon. A rectangle gives its four corners and an ellipse
    // a ring of points on it.
    SCOPEONE_CORE_EXPORT QList<QPoint> selectionCorners(const Markup& selection);
    SCOPEONE_CORE_EXPORT QList<QPoint> convexHull(const QList<QPoint>& points);
    // The outline of the selection grown by the given number of pixels, or shrunk when negative.
    // Empty when nothing is left.
    SCOPEONE_CORE_EXPORT QList<QPoint> enlargedOutline(int width, int height, const Markup& selection, double pixels);
    // The ellipse that has the same second moments as the selection, as a polygon
    SCOPEONE_CORE_EXPORT QList<QPoint> fittedEllipse(int width, int height, const Markup& selection);
    SCOPEONE_CORE_EXPORT QList<QPoint> rotatedCorners(const Markup& selection, double degrees);
    SCOPEONE_CORE_EXPORT QList<QPoint> scaledCorners(const Markup& selection, double factorX, double factorY);
    // The outline of the pixels that touch the seed and differ from it by at most tolerance
    SCOPEONE_CORE_EXPORT QList<QPoint> wandOutline(const ImageFrame& frame, const QPoint& seed, int tolerance);
    // Fewer corners, with every point of the outline within epsilon of the result
    SCOPEONE_CORE_EXPORT QList<QPoint> simplifiedOutline(const QList<QPoint>& points, double epsilon);

    // ---- frames

    // A frame like the given one in format and identity, with the given size and zeroed pixels
    SCOPEONE_CORE_EXPORT ImageFrame blankFrame(const ImageFrame& like, int width, int height);
    SCOPEONE_CORE_EXPORT std::vector<float> toFloat(const ImageFrame& frame);
    // Rounds and clamps to the range of the frame. The result has the format of like.
    SCOPEONE_CORE_EXPORT ImageFrame fromFloat(const std::vector<float>& values, const ImageFrame& like, int width, int height);

    // ---- stacks

    enum class Projection { Max, Average, Sum, Min, StdDev, Median };
    SCOPEONE_CORE_EXPORT ImageFrame zProject(const QList<ImageFrame>& frames, int first, int last, Projection method);
    SCOPEONE_CORE_EXPORT QList<ImageFrame> substack(const QList<ImageFrame>& frames, int first, int last, int step);
    SCOPEONE_CORE_EXPORT ImageFrame montage(const QList<ImageFrame>& frames, int columns, double scale, int border);

    enum class ProfileStat { Mean, Sum, Min, Max };
    // One value per frame, over the selection or over the whole frame when roi is null
    SCOPEONE_CORE_EXPORT QVector<double> zProfile(const QList<ImageFrame>& frames, const Markup* roi, ProfileStat stat);

    // ---- geometry

    SCOPEONE_CORE_EXPORT ImageFrame crop(const ImageFrame& frame, const QRect& area);
    SCOPEONE_CORE_EXPORT ImageFrame scaled(const ImageFrame& frame, double factorX, double factorY, bool smooth);
    // Turns the image by an angle in degrees, clockwise. With grow the image is made large
    // enough to hold the turned image.
    SCOPEONE_CORE_EXPORT ImageFrame rotated(const ImageFrame& frame, double degrees, bool smooth, bool grow);

    // ---- filters

    // Takes away the slowly changing background with a rolling ball of the given radius
    SCOPEONE_CORE_EXPORT ImageFrame subtractBackground(const ImageFrame& frame, double radius, bool lightBackground);

    // ---- contrast and noise

    // One count for every value that a frame can have, over all the given frames
    SCOPEONE_CORE_EXPORT std::vector<qint64> histogramOfFrames(const QList<ImageFrame>& frames);
    // The values between which all pixels lie but for the given percentage, which is split
    // between the dark and the bright end
    struct LevelRange
    {
        int low{0};
        int high{0};
    };
    SCOPEONE_CORE_EXPORT LevelRange saturatedRange(const std::vector<qint64>& histogram, double saturatedPercent);
    // Stretches the values from low to high over the whole range of the frame
    SCOPEONE_CORE_EXPORT ImageFrame stretched(const ImageFrame& frame, int low, int high);
    // Spreads the values so that all of them are about equally frequent. With sqrtCounts the
    // counts are square rooted first, which keeps the look more natural, as ImageJ does.
    SCOPEONE_CORE_EXPORT ImageFrame equalized(const ImageFrame& frame, bool sqrtCounts, const std::vector<qint64>* histogram = nullptr);
    // The new value of every value of the histogram after equalizing
    SCOPEONE_CORE_EXPORT std::vector<float> equalizationTable(const std::vector<qint64>& histogram, bool sqrtCounts);
    SCOPEONE_CORE_EXPORT ImageFrame withGaussianNoise(const ImageFrame& frame, double sigma);
    SCOPEONE_CORE_EXPORT ImageFrame withSaltAndPepper(const ImageFrame& frame, double percent);
    // Keeps what is smaller than the disc: bright detail on a dark background, or with
    // lightBackground dark detail on a bright one

    // ---- math


    enum class CalculatorOperation { Add, Subtract, Multiply, Divide, And, Or, Xor, Min, Max, Average, Difference };
    SCOPEONE_CORE_EXPORT ImageFrame calculate(const ImageFrame& first, const ImageFrame& second, CalculatorOperation operation);

    // ---- thresholds and binary images

    enum class ThresholdMethod { IsoData, Otsu, Mean, Triangle };
    SCOPEONE_CORE_EXPORT int autoThreshold(const std::vector<qint64>& histogram, ThresholdMethod method);
    SCOPEONE_CORE_EXPORT int autoThreshold(const ImageFrame& frame, ThresholdMethod method);
    // Pixels from low to high become 255 and the others 0, in an 8 bit frame
    SCOPEONE_CORE_EXPORT ImageFrame thresholded(const ImageFrame& frame, double low, double high);

    // ---- particles

    // A group of touching pixels of a binary image, found by following 8 connected pixels
    struct Particle
    {
        // Corners of the outline in pixels, which is the outer edge of the particle
        QList<QPoint> outline;
        std::vector<int> pixels;
        QRect bounds;
        bool touchesEdge{false};
        // Length of the outline, with the corners cut as ImageJ does
        double perimeter{0.0};
    };
    // Pixels above zero are the particles
    SCOPEONE_CORE_EXPORT QList<Particle> findParticles(const ImageFrame& mask);
    // Measures a particle on an image. A particle does not include the holes in it unless
    // includeHoles, in which case everything inside the outline counts.
    SCOPEONE_CORE_EXPORT Measurement measureParticle(const ImageFrame& image, const Particle& particle, double scale, bool includeHoles);
    // A frame in which every particle has its number, or 255, as its value
    SCOPEONE_CORE_EXPORT ImageFrame particleImage(const QList<Particle>& particles, int width, int height, bool numbered);
}
