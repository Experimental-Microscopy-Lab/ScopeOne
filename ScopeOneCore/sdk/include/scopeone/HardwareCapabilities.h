#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    class SCOPEONE_SDK_EXPORT DevicePropertyProvider
    {
    public:
        virtual ~DevicePropertyProvider();

        virtual QStringList listProperties(const QString& deviceId) = 0;
        virtual QString getProperty(const QString& deviceId,
                                    const QString& name,
                                    bool fromCache = false) = 0;
        virtual bool setProperty(const QString& deviceId,
                                 const QString& name,
                                 const QString& value,
                                 QString* errorMessage = nullptr) = 0;
        virtual QString getPropertyType(const QString& deviceId, const QString& name) = 0;
        virtual bool isPropertyReadOnly(const QString& deviceId, const QString& name) = 0;
        virtual bool isPropertyPreInit(const QString& deviceId, const QString& name) = 0;
        virtual QStringList getAllowedPropertyValues(const QString& deviceId,
                                                     const QString& name) = 0;
        virtual bool hasPropertyLimits(const QString& deviceId, const QString& name) = 0;
        virtual double getPropertyLowerLimit(const QString& deviceId, const QString& name) = 0;
        virtual double getPropertyUpperLimit(const QString& deviceId, const QString& name) = 0;

        // A property sequence steps a property through a list of values, one value on each
        // trigger of the camera, without a call per value. The capacity is the longest list the
        // device takes, 0 when the property cannot be sequenced.
        virtual long propertySequenceCapacity(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return 0;
        }
        // Loads the values and arms the property for the next triggers
        virtual bool startPropertySequence(const QString& deviceId,
                                           const QString& name,
                                           const QStringList& values,
                                           QString* errorMessage = nullptr)
        {
            (void)deviceId;
            (void)name;
            (void)values;
            if (errorMessage) *errorMessage = QStringLiteral("The property cannot be sequenced");
            return false;
        }
        virtual void stopPropertySequence(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
        }
    };

    class SCOPEONE_SDK_EXPORT StageProvider
    {
    public:
        virtual ~StageProvider();

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

    class SCOPEONE_SDK_EXPORT ShutterProvider
    {
    public:
        virtual ~ShutterProvider();

        virtual bool isShutterOpen(const QString& deviceId,
                                   bool& open,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool setShutterOpen(const QString& deviceId,
                                    bool open,
                                    QString* errorMessage = nullptr) = 0;

        // The shutter that lights the sample while a camera of the provider takes an image
        virtual QString defaultShutter() const
        {
            return {};
        }
        // Whether the provider opens the default shutter around each image by itself
        virtual bool autoShutter() const
        {
            return false;
        }
        virtual bool setAutoShutter(bool enabled, QString* errorMessage = nullptr)
        {
            if (errorMessage && enabled) *errorMessage = QStringLiteral("The provider has no automatic shutter");
            return !enabled;
        }
    };

    class SCOPEONE_SDK_EXPORT AutofocusProvider
    {
    public:
        virtual ~AutofocusProvider();

        virtual QString defaultAutofocus() const = 0;
        // Focuses with the autofocus device and returns once the focus has settled
        virtual bool runAutofocus(const QString& deviceId, QString* errorMessage = nullptr) = 0;
    };

    class SCOPEONE_SDK_EXPORT StateProvider
    {
    public:
        virtual ~StateProvider();

        virtual bool getState(const QString& deviceId,
                              long& state,
                              QString* errorMessage = nullptr) const = 0;
        virtual bool setState(const QString& deviceId,
                              long state,
                              QString* errorMessage = nullptr) = 0;
        virtual QString stateLabel(const QString& deviceId, long state) const = 0;
    };

    // One device property that a configuration preset sets
    struct ConfigSetting
    {
        QString device;
        QString property;
        QString value;
    };

    class SCOPEONE_SDK_EXPORT ConfigurationProvider
    {
    public:
        virtual ~ConfigurationProvider();

        // The device properties that applying a preset sets. Empty when the provider cannot
        // tell, which is not an error.
        virtual std::vector<ConfigSetting> configSettings(const QString& groupName,
                                                          const QString& configName) const
        {
            (void)groupName;
            (void)configName;
            return {};
        }

        virtual QStringList availableConfigGroups() const = 0;
        virtual QStringList availableConfigs(const QString& groupName) const = 0;
        virtual QString currentConfig(const QString& groupName) const = 0;
        virtual bool setConfig(const QString& groupName,
                               const QString& configName,
                               QString* errorMessage = nullptr) = 0;
    };
}
