#include "internal/SignalSourceManager.h"
#include "scopeone/PluginManifest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <utility>

namespace scopeone::core
{
    EventCountBinner::EventCountBinner(const QString& sourceId,
                                       const QString& quantity,
                                       const QString& unit,
                                       double tickPeriodSeconds,
                                       double sampleIntervalSeconds)
        : m_sourceId(sourceId)
          , m_quantity(quantity)
          , m_unit(unit)
          , m_tickPeriodSeconds(tickPeriodSeconds)
          , m_ticksPerSample(std::max<quint64>(
                1,
                static_cast<quint64>(std::llround(
                    sampleIntervalSeconds / tickPeriodSeconds))))
          , m_nextSampleTick(m_ticksPerSample)
          , m_sampleIntervalSeconds(
                static_cast<double>(m_ticksPerSample) * tickPeriodSeconds)
    {
    }

    void EventCountBinner::addEvent(quint64 tick)
    {
        const quint64 bin = binForTick(tick);
        advanceToBin(bin);
        if (bin == m_currentSample)
        {
            ++m_currentEventCount;
            ++m_totalInputEvents;
        }
    }

    void EventCountBinner::addMarker(quint64 tick, quint32 code)
    {
        advanceToTick(tick);
        m_pendingMarkers.append({static_cast<double>(tick) * m_tickPeriodSeconds, code});
        ++m_totalMarkers;
    }

    void EventCountBinner::advanceToTick(quint64 tick)
    {
        advanceToBin(binForTick(tick));
    }

    void EventCountBinner::advanceToElapsedSeconds(double elapsedSeconds)
    {
        if (elapsedSeconds > 0.0)
        {
            advanceToBin(static_cast<quint64>(elapsedSeconds / m_sampleIntervalSeconds));
        }
    }

    bool EventCountBinner::hasReadyChunks() const
    {
        return !m_readyChunks.isEmpty();
    }

    QList<TimeSeriesChunk> EventCountBinner::takeReadyChunks()
    {
        return std::exchange(m_readyChunks, QList<TimeSeriesChunk>{});
    }

    QList<TimeSeriesChunk> EventCountBinner::takeCompletedChunks()
    {
        finishChunk();
        return takeReadyChunks();
    }

    void EventCountBinner::appendCompletedValue(double value)
    {
        constexpr qsizetype kMaximumChunkSamples = 65536;
        m_completedValues.append(value);
        if (m_completedValues.size() >= kMaximumChunkSamples)
        {
            finishChunk();
        }
    }

    void EventCountBinner::finishChunk()
    {
        if (m_completedValues.isEmpty())
        {
            return;
        }
        TimeSeriesChunk chunk;
        chunk.sourceId = m_sourceId;
        chunk.quantity = m_quantity;
        chunk.unit = m_unit;
        chunk.startTimeSeconds = static_cast<double>(m_firstCompletedSample)
            * m_sampleIntervalSeconds;
        chunk.sampleIntervalSeconds = m_sampleIntervalSeconds;
        chunk.values.swap(m_completedValues);
        const double chunkEndTime = static_cast<double>(
            m_firstCompletedSample + static_cast<quint64>(chunk.values.size()))
            * m_sampleIntervalSeconds;
        int markerCount = 0;
        while (markerCount < m_pendingMarkers.size()
               && m_pendingMarkers[markerCount].timeSeconds < chunkEndTime)
        {
            ++markerCount;
        }
        if (markerCount > 0)
        {
            chunk.markers = m_pendingMarkers.mid(0, markerCount);
            m_pendingMarkers.remove(0, markerCount);
        }
        chunk.totalInputEvents = m_totalInputEvents;
        chunk.totalMarkers = m_totalMarkers;
        m_firstCompletedSample += static_cast<quint64>(chunk.values.size());
        m_readyChunks.append(std::move(chunk));
    }

    quint64 EventCountBinner::binForTick(quint64 tick) const
    {
        if (tick >= m_currentSampleTick && tick < m_nextSampleTick)
        {
            return m_currentSample;
        }
        return tick / m_ticksPerSample;
    }

    void EventCountBinner::advanceToBin(quint64 targetBin)
    {
        if (targetBin <= m_currentSample)
        {
            return;
        }
        while (m_currentSample < targetBin)
        {
            appendCompletedValue(static_cast<double>(m_currentEventCount));
            m_currentEventCount = 0;
            ++m_currentSample;
        }
        m_currentSampleTick = m_currentSample * m_ticksPerSample;
        m_nextSampleTick = m_currentSampleTick + m_ticksPerSample;
    }

    SignalSource::SignalSource(QObject* parent)
        : QObject(parent)
    {
    }

    SignalSource::~SignalSource() = default;
}

namespace scopeone::core::internal
{
    SignalSourceManager::SignalSourceManager(QObject* parent)
        : QObject(parent)
    {
        loadPlugins();
    }

    SignalSourceManager::~SignalSourceManager()
    {
        for (const QPointer<SignalSource>& source : std::as_const(m_sources))
        {
            delete source.data();
        }
        m_sources.clear();
        m_loaders.clear();
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
        SignalSource* source = sourceInstance(sourceId, errorMessage);
        return source && source->start(config, errorMessage);
    }

    void SignalSourceManager::stopTrace(const QString& sourceId)
    {
        if (SignalSource* source = m_sources.value(sourceId.trimmed()))
        {
            source->stop();
        }
    }

    SignalSourceState SignalSourceManager::state(const QString& sourceId) const
    {
        const QString id = sourceId.trimmed();
        const QPointer<SignalSource> source = m_sources.value(id);
        return source ? source->state() : m_states.value(id, SignalSourceState::Idle);
    }

    QString SignalSourceManager::stateMessage(const QString& sourceId) const
    {
        const QString id = sourceId.trimmed();
        const QPointer<SignalSource> source = m_sources.value(id);
        return source ? source->stateMessage()
                      : m_messages.value(id, QStringLiteral("Signal source is idle"));
    }

    void SignalSourceManager::loadPlugins()
    {
        const QStringList directories = {
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("plugins/hardware")),
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("plugins/hardware"))};

        for (const QString& path : directories)
        {
            const QDir directory(path);
            for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
            {
                if (!QLibrary::isLibrary(file.absoluteFilePath()))
                {
                    continue;
                }
                auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
                PluginManifest manifest;
                QString manifestError;
                if (!parsePluginManifest(
                        loader->metaData().value(QStringLiteral("MetaData")).toObject(),
                        PluginKind::Hardware,
                        manifest,
                        &manifestError))
                {
                    qWarning().noquote()
                        << QStringLiteral("Failed to load signal source plugin %1: %2")
                               .arg(file.fileName(), manifestError);
                    continue;
                }
                auto* plugin = qobject_cast<SignalSourcePlugin*>(loader->instance());
                if (!plugin)
                {
                    qWarning().noquote()
                        << QStringLiteral("Failed to load signal source plugin %1: %2")
                               .arg(file.fileName(), loader->errorString());
                    continue;
                }

                for (const SignalSourceDescriptor& descriptor : plugin->signalSources())
                {
                    const QString sourceId = descriptor.id.trimmed();
                    if (sourceId.isEmpty() || m_descriptors.contains(sourceId))
                    {
                        continue;
                    }
                    m_descriptors.insert(sourceId, descriptor);
                    m_plugins.insert(sourceId, plugin);
                    m_states.insert(sourceId, SignalSourceState::Idle);
                    m_messages.insert(sourceId, QStringLiteral("Signal source is idle"));
                }
                m_loaders.push_back(std::move(loader));
            }
        }
    }

    SignalSource* SignalSourceManager::sourceInstance(const QString& sourceId,
                                                       QString* errorMessage)
    {
        const QString id = sourceId.trimmed();
        if (SignalSource* existing = m_sources.value(id))
        {
            return existing;
        }
        SignalSourcePlugin* plugin = m_plugins.value(id);
        if (!plugin)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown signal source: %1").arg(id);
            }
            return nullptr;
        }
        SignalSource* source = plugin->createSignalSource(id, this);
        if (!source)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to create signal source: %1").arg(id);
            }
            return nullptr;
        }

        connect(source, &SignalSource::timeSeriesReady,
                this, &SignalSourceManager::timeSeriesReady);
        connect(source, &SignalSource::timestampedEventsReady,
                this, &SignalSourceManager::timestampedEventsReady);
        connect(source, &SignalSource::stateChanged,
                this, [this, id](SignalSourceState state, const QString& message)
                {
                    m_states.insert(id, state);
                    m_messages.insert(id, message);
                    emit sourceStateChanged(id, state, message);
                });
        connect(source, &SignalSource::sourceError,
                this, [this, id](const QString& message)
                {
                    emit sourceError(id, message);
                });
        m_sources.insert(id, source);
        return source;
    }
}
