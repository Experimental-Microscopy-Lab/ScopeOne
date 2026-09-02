#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "scopeone/DaqDevice.h"

class QPluginLoader;

namespace scopeone::core::internal
{
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
        void loadPlugins();
        DaqController* controller(const QString& deviceId,
                                  QString* errorMessage);

        std::vector<std::unique_ptr<QPluginLoader>> m_loaders;
        QHash<QString, DaqDevicePlugin*> m_plugins;
        QHash<QString, DaqDeviceDescriptor> m_descriptors;
        QHash<QString, QPointer<DaqController>> m_controllers;
        QHash<QString, DaqState> m_states;
        QHash<QString, QString> m_messages;
    };
}
