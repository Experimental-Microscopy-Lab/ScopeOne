#pragma once

#include <QString>

#include <optional>

namespace scopeone::core
{
    // A camera in ScopeOne's terms, as its provider reads it from the properties of its own system.
    // A field stays empty when the camera does not have it or its provider cannot tell.
    struct CameraSettings
    {
        double exposureMs{0.0};
        // 0 when unknown
        int binning{0};
        std::optional<double> gain;
        std::optional<double> offset;
        QString model;
        QString serialNumber;
    };
}
