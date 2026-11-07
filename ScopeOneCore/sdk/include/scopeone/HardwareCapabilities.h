#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    class SCOPEONE_SDK_EXPORT Stage
    {
    public:
        virtual ~Stage();

        virtual QString defaultXYStage() const = 0;
        virtual QString defaultZStage() const = 0;
        virtual bool getXYPosition(const QString& deviceId,
                                   double& x,
                                   double& y,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool getZPosition(const QString& deviceId,
                                  double& z,
                                  QString* errorMessage = nullptr) const = 0;
        virtual bool setRelativeXYPosition(const QString& deviceId,
                                           double dx,
                                           double dy,
                                           QString* errorMessage = nullptr) = 0;
        virtual bool setRelativeZPosition(const QString& deviceId,
                                          double dz,
                                          QString* errorMessage = nullptr) = 0;
        virtual bool setXYPosition(const QString& deviceId,
                                   double x,
                                   double y,
                                   QString* errorMessage = nullptr) = 0;
        virtual bool setZPosition(const QString& deviceId,
                                  double z,
                                  QString* errorMessage = nullptr) = 0;

        // A stage sequence steps a Z stage through a list of positions, one position on each
        // trigger of the camera. The capacity is the longest list the stage takes, 0 when it
        // cannot be sequenced.
        virtual long stageSequenceCapacity(const QString& deviceId) const
        {
            (void)deviceId;
            return 0;
        }
        // Loads the positions and arms the stage for the next triggers
        virtual bool startStageSequence(const QString& deviceId,
                                        const std::vector<double>& positions,
                                        QString* errorMessage = nullptr)
        {
            (void)deviceId;
            (void)positions;
            if (errorMessage) *errorMessage = QStringLiteral("The stage cannot be sequenced");
            return false;
        }
        virtual void stopStageSequence(const QString& deviceId)
        {
            (void)deviceId;
        }
    };

    class SCOPEONE_SDK_EXPORT Shutter
    {
    public:
        virtual ~Shutter();

        virtual bool isShutterOpen(const QString& deviceId,
                                   bool& open,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool setShutterOpen(const QString& deviceId,
                                    bool open,
                                    QString* errorMessage = nullptr) = 0;

        // The shutter that lights the sample while a camera of the device takes an image
        virtual QString defaultShutter() const
        {
            return {};
        }
        // Whether the device opens the default shutter around each image by itself
        virtual bool autoShutter() const
        {
            return false;
        }
        virtual bool setAutoShutter(bool enabled, QString* errorMessage = nullptr)
        {
            if (errorMessage && enabled) *errorMessage = QStringLiteral("The device has no automatic shutter");
            return !enabled;
        }
    };

    class SCOPEONE_SDK_EXPORT Autofocus
    {
    public:
        virtual ~Autofocus();

        virtual QString defaultAutofocus() const = 0;
        // Focuses with the autofocus device and returns once the focus has settled
        virtual bool runAutofocus(const QString& deviceId, QString* errorMessage = nullptr) = 0;
    };

    class SCOPEONE_SDK_EXPORT StateDevice
    {
    public:
        virtual ~StateDevice();

        virtual bool getState(const QString& deviceId,
                              long& state,
                              QString* errorMessage = nullptr) const = 0;
        virtual bool setState(const QString& deviceId,
                              long state,
                              QString* errorMessage = nullptr) = 0;
        virtual QString stateLabel(const QString& deviceId, long state) const = 0;
    };
}
