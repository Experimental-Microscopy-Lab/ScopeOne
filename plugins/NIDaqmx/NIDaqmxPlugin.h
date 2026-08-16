#pragma once

#include "scopeone/DaqDevice.h"

#include <QObject>

namespace scopeone::plugins
{
    class NIDaqmxPlugin final : public QObject,
                               public scopeone::core::DaqDevicePlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID SCOPEONE_DAQ_DEVICE_PLUGIN_IID)
        Q_INTERFACES(scopeone::core::DaqDevicePlugin)

    public:
        QList<scopeone::core::DaqDeviceDescriptor> devices() const override;
        scopeone::core::DaqController* createController(
            const QString& deviceId,
            QObject* parent = nullptr) override;
    };
}
