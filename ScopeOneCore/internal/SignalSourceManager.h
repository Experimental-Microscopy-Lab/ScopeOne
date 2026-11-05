#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

#include "scopeone/HardwareProvider.h"
#include "scopeone/SignalSource.h"
#include "scopeone/SignalSourceProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    // A signal source built into Core, such as the reader of PTU files
    class SignalSource : public QObject
    {
        Q_OBJECT

    public:
        explicit SignalSource(QObject* parent = nullptr);
        ~SignalSource() override;

        virtual bool start(const SignalAcquisitionConfig& config,
                           QString* errorMessage = nullptr) = 0;
        virtual void stop() = 0;
        virtual SignalSourceState state() const = 0;
        virtual QString stateMessage() const = 0;

    signals:
        void timeSeriesReady(const scopeone::core::TimeSeriesChunk& chunk);
        void timestampedEventsReady(const scopeone::core::TimestampedEventChunk& chunk);
        void stateChanged(scopeone::core::SignalSourceState state,
                          const QString& message);
        void sourceError(const QString& errorMessage);
    };

    // A kind of signal source built into Core, which lists its sources and creates them
    class SignalSourceFactory
    {
    public:
        virtual ~SignalSourceFactory() = default;
        virtual QList<SignalSourceDescriptor> signalSources() const = 0;
        virtual SignalSource* createSignalSource(const QString& sourceId,
                                                  QObject* parent = nullptr) = 0;
    };

    // The signal sources of Core and of the drivers under one list. The chunks of a
    // driver source arrive on a thread of its provider and are passed on in this object's thread.
    class SignalSourceManager final : public QObject
    {
        Q_OBJECT

    public:
        explicit SignalSourceManager(DeviceRegistry* registry, QObject* parent = nullptr);
        ~SignalSourceManager() override;

        QList<SignalSourceDescriptor> sources() const;
        bool startTrace(const SignalAcquisitionConfig& config,
                        QString* errorMessage = nullptr);
        void stopTrace(const QString& sourceId);
        SignalSourceState state(const QString& sourceId) const;
        QString stateMessage(const QString& sourceId) const;

    signals:
        void sourcesChanged();
        void timeSeriesReady(const scopeone::core::TimeSeriesChunk& chunk);
        void timestampedEventsReady(const scopeone::core::TimestampedEventChunk& chunk);
        void sourceStateChanged(const QString& sourceId,
                                scopeone::core::SignalSourceState state,
                                const QString& message);
        void sourceError(const QString& sourceId, const QString& errorMessage);

    private:
        void registerBuiltInSources();
        void refreshProviderSources();
        void detachProvider(const HardwareProviderPtr& provider);
        void updateState(const QString& sourceId, SignalSourceState state, const QString& message);
        SignalSource* sourceInstance(const QString& sourceId,
                                     QString* errorMessage);

        DeviceRegistry* m_registry{nullptr};
        std::vector<std::unique_ptr<SignalSourceFactory>> m_factories;
        QHash<QString, SignalSourceFactory*> m_factoryBySource;
        QHash<QString, HardwareProviderPtr> m_providerBySource;
        QHash<QString, HardwareProviderPtr> m_attachedProviders;
        QHash<QString, SignalSourceDescriptor> m_descriptors;
        QHash<QString, QPointer<SignalSource>> m_sources;
        QHash<QString, SignalSourceState> m_states;
        QHash<QString, QString> m_messages;
    };
}
