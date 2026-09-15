#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "scopeone/SignalSource.h"

class QPluginLoader;

namespace scopeone::core::internal
{
    class SignalSourceManager final : public QObject
    {
        Q_OBJECT

    public:
        explicit SignalSourceManager(QObject* parent = nullptr);
        ~SignalSourceManager() override;

        QList<SignalSourceDescriptor> sources() const;
        bool startTrace(const SignalAcquisitionConfig& config,
                        QString* errorMessage = nullptr);
        void stopTrace(const QString& sourceId);
        SignalSourceState state(const QString& sourceId) const;
        QString stateMessage(const QString& sourceId) const;

    signals:
        void timeSeriesReady(const scopeone::core::TimeSeriesChunk& chunk);
        void timestampedEventsReady(const scopeone::core::TimestampedEventChunk& chunk);
        void sourceStateChanged(const QString& sourceId,
                                scopeone::core::SignalSourceState state,
                                const QString& message);
        void sourceError(const QString& sourceId, const QString& errorMessage);

    private:
        void loadPlugins();
        SignalSource* sourceInstance(const QString& sourceId,
                                     QString* errorMessage);

        std::vector<std::unique_ptr<QPluginLoader>> m_loaders;
        QHash<QString, SignalSourcePlugin*> m_plugins;
        QHash<QString, SignalSourceDescriptor> m_descriptors;
        QHash<QString, QPointer<SignalSource>> m_sources;
        QHash<QString, SignalSourceState> m_states;
        QHash<QString, QString> m_messages;
    };
}
