#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "scopeone/SignalSource.h"

namespace scopeone::core::internal
{
    // A kind of signal source built into Core, which lists its sources and creates them
    class SignalSourceFactory
    {
    public:
        virtual ~SignalSourceFactory() = default;
        virtual QList<SignalSourceDescriptor> signalSources() const = 0;
        virtual SignalSource* createSignalSource(const QString& sourceId,
                                                  QObject* parent = nullptr) = 0;
    };

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
        void registerSources();
        SignalSource* sourceInstance(const QString& sourceId,
                                     QString* errorMessage);

        std::vector<std::unique_ptr<SignalSourceFactory>> m_factories;
        QHash<QString, SignalSourceFactory*> m_factoryBySource;
        QHash<QString, SignalSourceDescriptor> m_descriptors;
        QHash<QString, QPointer<SignalSource>> m_sources;
        QHash<QString, SignalSourceState> m_states;
        QHash<QString, QString> m_messages;
    };
}
