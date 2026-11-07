#include "internal/SignalSourceManager.h"

#include "internal/HardwareRuntime.h"

#include <QDebug>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace scopeone::core::internal
{
    SignalSourceManager::SignalSourceManager(DeviceRegistry* registry, QObject* parent)
        : QObject(parent)
          , m_registry(registry)
    {
        connect(m_registry, &DeviceRegistry::changed, this, &SignalSourceManager::refreshProviderSources);
        refreshProviderSources();
    }

    SignalSourceManager::~SignalSourceManager()
    {
        for (const DevicePtr& provider : std::as_const(m_attachedProviders))
        {
            detachProvider(provider);
        }
    }

    QList<SignalSourceDescriptor> SignalSourceManager::sources() const
    {
        QList<SignalSourceDescriptor> result = m_descriptors.values();
        std::sort(result.begin(), result.end(),
                  [](const SignalSourceDescriptor& left,
                     const SignalSourceDescriptor& right)
                  {
                      return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
                  });
        return result;
    }

    bool SignalSourceManager::startTrace(const SignalAcquisitionConfig& config,
                                         QString* errorMessage)
    {
        const QString sourceId = config.sourceId.trimmed();
        if (!m_descriptors.contains(sourceId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown signal source: %1").arg(sourceId);
            }
            return false;
        }
        SignalAcquisitionConfig normalized = config;
        normalized.sourceId = sourceId;
        const DevicePtr provider = m_providerBySource.value(sourceId);
        return dynamic_cast<SignalSource*>(provider.get())->startSignal(normalized, errorMessage);
    }

    void SignalSourceManager::stopTrace(const QString& sourceId)
    {
        const QString id = sourceId.trimmed();
        if (const DevicePtr provider = m_providerBySource.value(id))
        {
            dynamic_cast<SignalSource*>(provider.get())->stopSignal(id);
        }
    }

    SignalSourceState SignalSourceManager::state(const QString& sourceId) const
    {
        return m_states.value(sourceId.trimmed(), SignalSourceState::Idle);
    }

    QString SignalSourceManager::stateMessage(const QString& sourceId) const
    {
        return m_messages.value(sourceId.trimmed(), QStringLiteral("Signal source is idle"));
    }

    // Follows the drivers as they are registered and removed: the sources of each
    // driver that provides signal sources join the list, and their chunks are passed on
    void SignalSourceManager::refreshProviderSources()
    {
        QHash<QString, DevicePtr> current;
        for (const HardwareProviderDescriptor& descriptor : m_registry->providers())
        {
            const DevicePtr provider = m_registry->provider(descriptor.id);
            if (dynamic_cast<SignalSource*>(provider.get()))
            {
                current.insert(descriptor.id, provider);
            }
        }
        bool changed = false;
        for (auto it = m_attachedProviders.begin(); it != m_attachedProviders.end();)
        {
            if (current.value(it.key()) == it.value())
            {
                ++it;
                continue;
            }
            detachProvider(it.value());
            it = m_attachedProviders.erase(it);
            changed = true;
        }
        for (auto it = current.cbegin(); it != current.cend(); ++it)
        {
            if (m_attachedProviders.contains(it.key()))
            {
                continue;
            }
            auto* const signalProvider = dynamic_cast<SignalSource*>(it.value().get());
            const QPointer<SignalSourceManager> self(this);
            signalProvider->setSignalSinks(
                [self](const TimeSeriesChunk& chunk)
                {
                    QMetaObject::invokeMethod(self, [self, chunk]() { if (self) emit self->timeSeriesReady(chunk); },
                                              Qt::QueuedConnection);
                },
                [self](const TimestampedEventChunk& chunk)
                {
                    QMetaObject::invokeMethod(self, [self, chunk]() { if (self) emit self->timestampedEventsReady(chunk); },
                                              Qt::QueuedConnection);
                },
                [self](const QString& sourceId, SignalSourceState state, const QString& message)
                {
                    QMetaObject::invokeMethod(self, [self, sourceId, state, message]()
                    {
                        if (self) self->updateState(sourceId, state, message);
                    }, Qt::QueuedConnection);
                });
            for (const SignalSourceDescriptor& descriptor : signalProvider->signalSources())
            {
                const QString sourceId = descriptor.id.trimmed();
                if (sourceId.isEmpty() || m_descriptors.contains(sourceId))
                {
                    qWarning().noquote() << QStringLiteral("Hardware provider %1: invalid or duplicate signal source %2")
                                                .arg(it.key(), descriptor.id);
                    continue;
                }
                m_descriptors.insert(sourceId, descriptor);
                m_providerBySource.insert(sourceId, it.value());
                m_states.insert(sourceId, SignalSourceState::Idle);
                m_messages.insert(sourceId, QStringLiteral("Signal source is idle"));
            }
            m_attachedProviders.insert(it.key(), it.value());
            changed = true;
        }
        if (changed)
        {
            emit sourcesChanged();
        }
    }

    void SignalSourceManager::detachProvider(const DevicePtr& provider)
    {
        dynamic_cast<SignalSource*>(provider.get())->setSignalSinks({}, {}, {});
        for (auto it = m_providerBySource.begin(); it != m_providerBySource.end();)
        {
            if (it.value() != provider)
            {
                ++it;
                continue;
            }
            m_descriptors.remove(it.key());
            m_states.remove(it.key());
            m_messages.remove(it.key());
            it = m_providerBySource.erase(it);
        }
    }

    void SignalSourceManager::updateState(const QString& sourceId,
                                          SignalSourceState state,
                                          const QString& message)
    {
        if (!m_providerBySource.contains(sourceId))
        {
            return;
        }
        m_states.insert(sourceId, state);
        m_messages.insert(sourceId, message);
        emit sourceStateChanged(sourceId, state, message);
        if (state == SignalSourceState::Error)
        {
            emit sourceError(sourceId, message);
        }
    }
}
