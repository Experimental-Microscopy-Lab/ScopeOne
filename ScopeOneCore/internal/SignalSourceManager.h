#pragma once

#include <QHash>
#include <QObject>

#include "scopeone/HardwareProvider.h"
#include "scopeone/SignalSource.h"
#include "scopeone/SignalSourceProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    // The signal sources of the drivers under one list. Their chunks arrive on a thread of
    // the provider and are passed on in this object's thread.
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
        void refreshProviderSources();
        void detachProvider(const HardwareProviderPtr& provider);
        void updateState(const QString& sourceId, SignalSourceState state, const QString& message);

        DeviceRegistry* m_registry{nullptr};
        QHash<QString, HardwareProviderPtr> m_providerBySource;
        QHash<QString, HardwareProviderPtr> m_attachedProviders;
        QHash<QString, SignalSourceDescriptor> m_descriptors;
        QHash<QString, SignalSourceState> m_states;
        QHash<QString, QString> m_messages;
    };
}
