#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

#include "scopeone/HardwareTypes.h"
#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    // What a driver creates by device name. A device implements the capability interfaces that
    // fit it, such as Camera or ZStage, next to this class, and ScopeOne finds them by casting.
    // Every call that names a deviceId means one of the devices() of the object, which tells
    // the peripherals of a hub apart. The property methods default to a device without
    // properties.
    class SCOPEONE_SDK_EXPORT Device
    {
    public:
        virtual ~Device();

        virtual HardwareProviderDescriptor descriptor() const = 0;
        // The devices the object exposes. A Hub lists its peripherals, and so does the default.
        virtual QList<HardwareDeviceDescriptor> devices() const;

        virtual QStringList listProperties(const QString& deviceId)
        {
            (void)deviceId;
            return {};
        }
        virtual QString getProperty(const QString& deviceId,
                                    const QString& name,
                                    bool fromCache = false)
        {
            (void)deviceId;
            (void)name;
            (void)fromCache;
            return {};
        }
        virtual bool setProperty(const QString& deviceId,
                                 const QString& name,
                                 const QString& value,
                                 QString* errorMessage = nullptr)
        {
            (void)deviceId;
            (void)value;
            if (errorMessage) *errorMessage = QStringLiteral("Unknown property %1").arg(name);
            return false;
        }
        virtual QString getPropertyType(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return {};
        }
        virtual bool isPropertyReadOnly(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return true;
        }
        virtual bool isPropertyPreInit(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return false;
        }
        virtual QStringList getAllowedPropertyValues(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return {};
        }
        virtual bool hasPropertyLimits(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return false;
        }
        virtual double getPropertyLowerLimit(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return 0.0;
        }
        virtual double getPropertyUpperLimit(const QString& deviceId, const QString& name)
        {
            (void)deviceId;
            (void)name;
            return 0.0;
        }

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

    // A device that has peripherals, such as a DAQ board with its channels. The peripherals are
    // the devices() of the hub, each with its own deviceId.
    class SCOPEONE_SDK_EXPORT Hub
    {
    public:
        virtual ~Hub();

        virtual QList<HardwareDeviceDescriptor> peripherals() const = 0;
    };

    using DevicePtr = std::shared_ptr<Device>;
}
