#pragma once

#include <QString>

namespace scopeone::core::internal
{
    // Hardware functions of an acquisition that act on the core of Micro-Manager rather than on
    // one device: the shutter that the core opens for every image, and the autofocus device
    class AcquisitionHardware
    {
    public:
        virtual ~AcquisitionHardware() = default;

        // Whether the core opens the shutter around each image by itself
        virtual bool autoShutter(bool& enabled, QString* errorMessage = nullptr) const = 0;
        virtual bool setAutoShutter(bool enabled, QString* errorMessage = nullptr) = 0;
        // Opens or closes the shutter that the core currently uses
        virtual bool setCoreShutterOpen(bool open, QString* errorMessage = nullptr) = 0;

        // Autofocus device that the core currently uses
        virtual bool hasAutofocus() const = 0;
        virtual bool runAutofocus(QString* errorMessage = nullptr) = 0;
    };
}
