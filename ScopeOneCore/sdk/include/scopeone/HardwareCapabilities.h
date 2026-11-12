#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    // XYStage and ZStage are separate devices, as in Micro-Manager, so a device without
    // peripherals is either kind after the interface it implements. Which stage, shutter or
    // autofocus the acquisitions use are Core roles of the cfg file, not chosen by a device.
    class SCOPEONE_SDK_EXPORT XYStage
    {
    public:
        virtual ~XYStage();

        virtual bool getXYPosition(const QString& deviceId,
                                   double& x,
                                   double& y,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool setRelativeXYPosition(const QString& deviceId,
                                           double dx,
                                           double dy,
                                           QString* errorMessage = nullptr) = 0;
        virtual bool setXYPosition(const QString& deviceId,
                                   double x,
                                   double y,
                                   QString* errorMessage = nullptr) = 0;
    };

    class SCOPEONE_SDK_EXPORT ZStage
    {
    public:
        virtual ~ZStage();

        virtual bool getZPosition(const QString& deviceId,
                                  double& z,
                                  QString* errorMessage = nullptr) const = 0;
        virtual bool setRelativeZPosition(const QString& deviceId,
                                          double dz,
                                          QString* errorMessage = nullptr) = 0;
        virtual bool setZPosition(const QString& deviceId,
                                  double z,
                                  QString* errorMessage = nullptr) = 0;

        // A stage sequence steps the stage through a list of positions, one position on each
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
    };

    class SCOPEONE_SDK_EXPORT Autofocus
    {
    public:
        virtual ~Autofocus();

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
