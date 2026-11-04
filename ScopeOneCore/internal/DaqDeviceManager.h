#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "scopeone/DaqDevice.h"

namespace scopeone::core::internal
{
    // A kind of DAQ device built into Core, which lists the devices it finds and drives them
    class DaqDeviceFactory
    {
    public:
        virtual ~DaqDeviceFactory() = default;
        virtual QList<DaqDeviceDescriptor> devices() const = 0;
        virtual DaqController* createController(const QString& deviceId,
                                                 QObject* parent = nullptr) = 0;
    };

    class DaqDeviceManager final : public QObject
    {
        Q_OBJECT

    public:
        explicit DaqDeviceManager(QObject* parent = nullptr);
        ~DaqDeviceManager() override;

        QList<DaqDeviceDescriptor> devices() const;
        bool start(const DaqSessionConfig& config,
                   QString* errorMessage = nullptr);
        void stop(const QString& deviceId);
        DaqState state(const QString& deviceId) const;
        QString stateMessage(const QString& deviceId) const;

    signals:
        void stateChanged(const QString& deviceId,
                          scopeone::core::DaqState state,
                          const QString& message);
        void deviceError(const QString& deviceId, const QString& errorMessage);
        void inputDataReady(const scopeone::core::DaqInputChunk& chunk);

    private:
        void registerDevices();
        DaqController* controller(const QString& deviceId,
                                  QString* errorMessage);

        std::vector<std::unique_ptr<DaqDeviceFactory>> m_factories;
        mutable bool m_devicesRegistered{false};
        QHash<QString, DaqDeviceFactory*> m_factoryByDevice;
        QHash<QString, DaqDeviceDescriptor> m_descriptors;
        QHash<QString, QPointer<DaqController>> m_controllers;
        QHash<QString, DaqState> m_states;
        QHash<QString, QString> m_messages;
    };
}
