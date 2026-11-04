#pragma once

#include <QHash>
#include <QObject>

#include "scopeone/DaqDevice.h"
#include "scopeone/DaqProvider.h"
#include "scopeone/HardwareProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    // The DAQ devices of the drivers. Their inputs and states arrive on a thread of the
    // provider and are passed on in this object's thread.
    class DaqDeviceManager final : public QObject
    {
        Q_OBJECT

    public:
        explicit DaqDeviceManager(DeviceRegistry* registry, QObject* parent = nullptr);
        ~DaqDeviceManager() override;

        QList<DaqDeviceDescriptor> devices() const;
        bool start(const DaqSessionConfig& config,
                   QString* errorMessage = nullptr);
        void stop(const QString& deviceId);
        DaqState state(const QString& deviceId) const;
        QString stateMessage(const QString& deviceId) const;

    signals:
        void devicesChanged();
        void stateChanged(const QString& deviceId,
                          scopeone::core::DaqState state,
                          const QString& message);
        void deviceError(const QString& deviceId, const QString& errorMessage);
        void inputDataReady(const scopeone::core::DaqInputChunk& chunk);

    private:
        void refreshProviderDevices();
        void detachProvider(const HardwareProviderPtr& provider);
        void updateState(const QString& deviceId, DaqState state, const QString& message);

        DeviceRegistry* m_registry{nullptr};
        QHash<QString, HardwareProviderPtr> m_attachedProviders;
        QHash<QString, HardwareProviderPtr> m_providerByDevice;
        QHash<QString, DaqDeviceDescriptor> m_descriptors;
        QHash<QString, DaqState> m_states;
        QHash<QString, QString> m_messages;
    };
}
