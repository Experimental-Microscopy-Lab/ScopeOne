#pragma once

#include "scopeone/ExperimentDocument.h"
#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

#include <QPoint>
#include <QRect>

namespace scopeone::core
{
    // Pixel statistics of one region of interest in image coordinates
    struct RoiStatistics
    {
        qint64 pixelCount{0};
        double mean{0.0};
        double stdDev{0.0};
        int minValue{0};
        int maxValue{0};

        bool hasData() const { return pixelCount > 0; }
    };

    // Shape of a region of interest using the scene markup geometry conventions
    struct RoiShape
    {
        DocumentMarkupType type{DocumentMarkupType::Rect};
        QPoint start;
        QPoint end;
        QRect rect;
    };

    // Statistics over the pixels covered by a rectangle or line ROI
    SCOPEONE_CORE_EXPORT bool measureRoi(const ImageFrame& frame,
                                         const RoiShape& shape,
                                         RoiStatistics& statistics);
} // namespace scopeone::core
