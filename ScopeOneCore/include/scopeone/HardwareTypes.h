#pragma once

#include <QMetaType>
#include <QString>
#include <QVariantMap>

#include <cstdint>

#include "scopeone/scopeone_core_export.h"

namespace scopeone::core
{
    enum class HardwareDeviceKind
    {
        Unknown,
        Camera,
        XYStage,
        ZStage,
        Shutter,
        State,
        Hub
    };

    enum class HardwareDeviceState
    {
        Unknown,
        Discovered,
        Initialized,
        Faulted,
        Unavailable
    };

    enum class HardwareEndpointKind
    {
        InProcess,
        DriverHost
    };

    struct SCOPEONE_CORE_EXPORT HardwareProviderDescriptor
    {
        QString id;
        QString name;
        QString version;
    };

    struct SCOPEONE_CORE_EXPORT HardwareDeviceDescriptor
    {
        QString logicalId;
        QString providerId;
        QString providerDeviceId;
        QString hardwareId;
        QString name;
        HardwareDeviceKind kind{HardwareDeviceKind::Unknown};
        HardwareDeviceState state{HardwareDeviceState::Unknown};
        HardwareEndpointKind endpoint{HardwareEndpointKind::InProcess};
        QVariantMap properties;
    };

    struct SCOPEONE_CORE_EXPORT ClockStamp
    {
        std::int64_t ticks{0};
        std::int64_t tickPeriodNumerator{1};
        std::int64_t tickPeriodDenominator{1000000000};
        std::int64_t hostMonotonicNs{0};
        QString clockDomain;
        QString source;

        bool isValid() const
        {
            return tickPeriodNumerator > 0
                && tickPeriodDenominator > 0
                && !clockDomain.isEmpty()
                && !source.isEmpty();
        }
    };
}

Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceKind)
Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceState)
Q_DECLARE_METATYPE(scopeone::core::HardwareEndpointKind)
Q_DECLARE_METATYPE(scopeone::core::HardwareProviderDescriptor)
Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceDescriptor)
Q_DECLARE_METATYPE(scopeone::core::ClockStamp)
