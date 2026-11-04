#pragma once

#include <QMetaType>
#include <QString>
#include <QVariantMap>

#include "scopeone/scopeone_sdk_export.h"

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
        Hub,
        Serial,
        Generic,
        AutoFocus,
        ImageProcessor,
        SignalIO,
        Magnifier,
        SLM,
        Galvo,
        PressurePump,
        VolumetricPump
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

    struct SCOPEONE_SDK_EXPORT HardwareProviderDescriptor
    {
        QString id;
        QString name;
        QString version;
    };

    struct SCOPEONE_SDK_EXPORT HardwareDeviceDescriptor
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

}

Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceKind)
Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceState)
Q_DECLARE_METATYPE(scopeone::core::HardwareEndpointKind)
Q_DECLARE_METATYPE(scopeone::core::HardwareProviderDescriptor)
Q_DECLARE_METATYPE(scopeone::core::HardwareDeviceDescriptor)
