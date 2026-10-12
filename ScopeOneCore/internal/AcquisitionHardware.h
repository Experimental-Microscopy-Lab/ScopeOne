#pragma once

#include <QString>

#include <vector>

namespace scopeone::core::internal
{
    // Hardware functions of an acquisition that act on the core of Micro-Manager rather than on
    // one device: the shutter that the core opens for every image, and the autofocus device
    // The values that one device property takes on successive triggers
    struct PropertySequence
    {
        QString device;
        QString property;
        std::vector<QString> values;
    };

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

        // Number of positions that the focus device can step through by itself on each
        // trigger, or 0 when it has no hardware sequence
        virtual long focusSequenceCapacity(const QString& focusDevice) const = 0;
        // Loads the positions into the focus device and arms it, so that every trigger of the
        // camera moves it to the next one
        virtual bool startFocusSequence(const QString& focusDevice,
                                        const std::vector<double>& positions,
                                        QString* errorMessage = nullptr) = 0;
        virtual void stopFocusSequence(const QString& focusDevice) = 0;

        // Number of values that a device property can step through by itself on each
        // trigger, or 0 when it has no hardware sequence
        virtual long propertySequenceCapacity(const QString& device, const QString& property) const = 0;
        // Loads the values of the properties and arms them, so that every trigger of the
        // camera sets the next value. Nothing stays armed when it fails.
        virtual bool startPropertySequences(const std::vector<PropertySequence>& sequences,
                                            QString* errorMessage = nullptr) = 0;
        virtual void stopPropertySequences(const std::vector<PropertySequence>& sequences) = 0;
    };
}
