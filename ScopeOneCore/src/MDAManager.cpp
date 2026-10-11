#include "internal/MDAManager.h"

#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <future>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        quint64 currentTimestampNs()
        {
            return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }
    }

    // Creates the MDA manager and registers queued signal types
    MDAManager::MDAManager(QObject* parent)
        : QObject(parent)
    {
        qRegisterMetaType<scopeone::core::internal::MDAOutput>("scopeone::core::internal::MDAOutput");
        m_threadPool.setMaxThreadCount(1);
    }

    // Cancels and joins the acquisition worker before teardown
    MDAManager::~MDAManager()
    {
        cancelAndWait();
    }

    // Connects MDA capture to the active camera backend
    void MDAManager::setCameraProvider(CameraProvider* cameraProvider)
    {
        m_cameraProvider = cameraProvider;
    }

    void MDAManager::setStageProvider(StageProvider* stageProvider)
    {
        m_stageProvider = stageProvider;
    }

    void MDAManager::setConfigurationProvider(ConfigurationProvider* configurationProvider)
    {
        m_configurationProvider = configurationProvider;
    }

    void MDAManager::setAcquisitionHardware(AcquisitionHardware* hardware)
    {
        m_acquisitionHardware = hardware;
    }

    // Starts one immutable acquisition event sequence
    bool MDAManager::start(const QList<AcquisitionEvent>& events, bool block)
    {
        if (m_running.exchange(true))
        {
            emit sequenceError(QStringLiteral("MDA already running"));
            return false;
        }
        if (events.isEmpty())
        {
            m_running.store(false);
            emit sequenceError(QStringLiteral("MDA event list is empty"));
            return false;
        }

        m_cancelRequested.store(false);
        if (block)
        {
            runSequence(events);
            return true;
        }

        m_threadPool.start([this, events]()
        {
            runSequence(events);
        });
        return true;
    }

    // Requests cancellation for the active MDA run
    void MDAManager::requestCancel()
    {
        m_cancelRequested.store(true);
    }

    // Cancels and joins the active acquisition worker
    void MDAManager::cancelAndWait()
    {
        requestCancel();
        m_threadPool.waitForDone();
    }

    // Executes a precomputed event sequence in order
    void MDAManager::runSequence(QList<AcquisitionEvent> events)
    {
        if (!m_cameraProvider)
        {
            m_running.store(false);
            emit sequenceError(QStringLiteral("Camera provider not available"));
            return;
        }

        QElapsedTimer timer;
        timer.start();
        const qint64 sequenceOriginMs = events.first().minimumStartTimeMs;
        SequenceState state;
        captureInitialState(events, state);

        for (const AcquisitionEvent& event : events)
        {
            const qint64 targetMs = event.minimumStartTimeMs - sequenceOriginMs;
            while (timer.elapsed() < targetMs)
            {
                if (m_cancelRequested.load())
                {
                    restoreInitialState(state);
                    m_running.store(false);
                    emit sequenceCanceled();
                    return;
                }
                const qint64 remaining = targetMs - timer.elapsed();
                QThread::msleep(static_cast<unsigned long>((std::min<qint64>)(5, remaining)));
            }

            if (m_cancelRequested.load())
            {
                restoreInitialState(state);
                m_running.store(false);
                emit sequenceCanceled();
                return;
            }

            MDAOutput output;
            output.event = event;
            output.startedTimestampNs = currentTimestampNs();
            QString errorMessage;
            output.succeeded = setupEvent(event, state, &errorMessage)
                && openShutter(event, state, &errorMessage)
                && execEvent(event, output, &errorMessage);
            closeShutter(event, state);
            output.completedTimestampNs = currentTimestampNs();
            output.errorMessage = output.succeeded
                                      ? QString()
                                      : (errorMessage.isEmpty()
                                             ? QStringLiteral("Failed to execute acquisition event")
                                             : errorMessage);
            emit eventFinished(output);

            if (!output.succeeded)
            {
                restoreInitialState(state);
                m_running.store(false);
                emit sequenceError(output.errorMessage);
                return;
            }
        }

        restoreInitialState(state);
        m_running.store(false);
        emit sequenceFinished();
    }

    // Remembers what the hardware is set to before the sequence, so it can be put back
    void MDAManager::captureInitialState(const QList<AcquisitionEvent>& events, SequenceState& state)
    {
        bool usesXY = false;
        bool usesZ = false;
        QString channelGroup;
        for (const AcquisitionEvent& event : events)
        {
            usesXY = usesXY || event.hasXY;
            usesZ = usesZ || event.hasZ || event.zOffsetUm != 0.0 || event.autofocusBefore;
            if (channelGroup.isEmpty() && !event.channelPreset.isEmpty())
            {
                channelGroup = event.channelGroup;
            }
        }
        if (m_stageProvider && usesXY)
        {
            const QString stage = m_stageProvider->defaultXYStage();
            state.hasInitialXY = !stage.isEmpty()
                && m_stageProvider->getXYPosition(stage, state.initialX, state.initialY);
        }
        if (m_stageProvider && usesZ)
        {
            const QString focus = m_stageProvider->defaultZStage();
            state.hasInitialZ = !focus.isEmpty()
                && m_stageProvider->getZPosition(focus, state.initialZ);
            state.zReference = state.initialZ;
        }
        if (m_configurationProvider && !channelGroup.isEmpty())
        {
            state.initialChannelGroup = channelGroup;
            state.initialChannelPreset = m_configurationProvider->currentConfig(channelGroup);
        }
        // Like Micro-Manager, only take over the shutter when the core controls it
        const bool wantsShutter = std::any_of(events.begin(), events.end(),
                                              [](const AcquisitionEvent& event)
                                              {
                                                  return event.openShutterBefore;
                                              });
        bool autoShutterOn = false;
        state.holdShutter = wantsShutter && m_acquisitionHardware
            && m_acquisitionHardware->autoShutter(autoShutterOn) && autoShutterOn;
    }

    // Opens the shutter at the start of a block of events and keeps it open until the block ends
    bool MDAManager::openShutter(const AcquisitionEvent& event,
                                 SequenceState& state,
                                 QString* errorMessage)
    {
        if (!state.holdShutter || !event.openShutterBefore || state.shutterOpen)
        {
            return true;
        }
        QString error;
        if (!m_acquisitionHardware->setAutoShutter(false, &error)
            || !m_acquisitionHardware->setCoreShutterOpen(true, &error))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to open the shutter: %1").arg(error);
            }
            m_acquisitionHardware->setAutoShutter(true);
            return false;
        }
        state.shutterOpen = true;
        return true;
    }

    // Closes the shutter after the last event of a block and gives it back to the core
    void MDAManager::closeShutter(const AcquisitionEvent& event, SequenceState& state)
    {
        if (!state.holdShutter || !state.shutterOpen || !event.closeShutterAfter)
        {
            return;
        }
        QString error;
        if (!m_acquisitionHardware->setCoreShutterOpen(false, &error))
        {
            qWarning().noquote() << "Failed to close the shutter:" << error;
        }
        m_acquisitionHardware->setAutoShutter(true);
        state.shutterOpen = false;
    }

    // Puts the stages and the channel back where they were before the sequence
    void MDAManager::restoreInitialState(const SequenceState& state)
    {
        QString error;
        if (state.holdShutter && state.shutterOpen && m_acquisitionHardware)
        {
            m_acquisitionHardware->setCoreShutterOpen(false);
            m_acquisitionHardware->setAutoShutter(true);
        }
        if (state.hasInitialXY && m_stageProvider)
        {
            const QString stage = m_stageProvider->defaultXYStage();
            if (!stage.isEmpty()
                && !m_stageProvider->setXYPosition(stage, state.initialX, state.initialY, &error))
            {
                qWarning().noquote() << "Failed to restore the XY position:" << error;
            }
        }
        if (state.hasInitialZ && m_stageProvider)
        {
            const QString focus = m_stageProvider->defaultZStage();
            if (!focus.isEmpty()
                && !m_stageProvider->setZPosition(focus, state.initialZ, &error))
            {
                qWarning().noquote() << "Failed to restore the Z position:" << error;
            }
        }
        if (!state.initialChannelPreset.isEmpty() && m_configurationProvider
            && state.initialChannelPreset != state.channelPreset
            && !m_configurationProvider->setConfig(state.initialChannelGroup,
                                                   state.initialChannelPreset,
                                                   &error))
        {
            qWarning().noquote() << "Failed to restore the channel:" << error;
        }
    }

    // Moves hardware into place before capture. Settings that did not change since the
    // previous event are not sent again.
    bool MDAManager::setupEvent(const AcquisitionEvent& event,
                                SequenceState& state,
                                QString* errorMessage)
    {
        state.channelChanged = false;
        // The exposure goes first because a channel preset may change it again
        const bool exposureChanged = !state.exposureKnown
            || state.exposureMs != event.exposureMs
            || state.exposureCameras != event.cameraIds;
        if (event.exposureMs > 0.0 && exposureChanged
            && !setExposure(event.cameraIds, event.exposureMs, errorMessage))
        {
            return false;
        }
        if (event.exposureMs > 0.0)
        {
            state.exposureKnown = true;
            state.exposureMs = event.exposureMs;
            state.exposureCameras = event.cameraIds;
        }
        if (!applyChannel(event, state, errorMessage))
        {
            return false;
        }
        if (state.channelChanged && event.exposureMs > 0.0
            && !setExposure(event.cameraIds, event.exposureMs, errorMessage))
        {
            return false;
        }
        if (event.hasXY && (!state.xyKnown || state.x != event.x || state.y != event.y))
        {
            if (!moveXY(event.x, event.y, errorMessage))
            {
                return false;
            }
            state.xyKnown = true;
            state.x = event.x;
            state.y = event.y;
        }
        if (event.autofocusBefore && !runAutofocus(event, state, errorMessage))
        {
            return false;
        }
        // Z positions follow the autofocus result of their position
        const double targetZ = event.z + state.focusShift.value(event.positionIndex, 0.0);
        if (event.hasZ && (!state.zKnown || state.z != targetZ))
        {
            if (!moveZ(targetZ, errorMessage))
            {
                return false;
            }
            state.zKnown = true;
            state.z = targetZ;
        }
        if (!event.hasZ && !applyZOffset(event, state, errorMessage))
        {
            return false;
        }

        return true;
    }

    // Applies the configuration preset of the channel when it differs from the previous event
    bool MDAManager::applyChannel(const AcquisitionEvent& event,
                                  SequenceState& state,
                                  QString* errorMessage)
    {
        if (event.channelPreset.isEmpty()
            || (event.channelGroup == state.channelGroup && event.channelPreset == state.channelPreset))
        {
            return true;
        }
        if (!m_configurationProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Configuration provider not available");
            return false;
        }
        QString error;
        if (!m_configurationProvider->setConfig(event.channelGroup, event.channelPreset, &error))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to apply channel %1 of group %2: %3")
                                    .arg(event.channelPreset, event.channelGroup, error);
            }
            return false;
        }
        state.channelGroup = event.channelGroup;
        state.channelPreset = event.channelPreset;
        state.channelChanged = true;
        return true;
    }

    // Moves focus by the channel offset from the focus position the sequence began at. This
    // is only used without Z positions, which carry the offset in their own coordinates.
    bool MDAManager::applyZOffset(const AcquisitionEvent& event,
                                  SequenceState& state,
                                  QString* errorMessage)
    {
        if (event.zOffsetUm == state.zOffset)
        {
            return true;
        }
        if (!m_stageProvider || !state.hasInitialZ)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Focus position is not available");
            return false;
        }
        if (!moveZ(state.zReference + event.zOffsetUm, errorMessage))
        {
            return false;
        }
        state.zOffset = event.zOffsetUm;
        state.zKnown = true;
        state.z = state.zReference + event.zOffsetUm;
        return true;
    }

    // Focuses with the autofocus device at the position of the event. The focus is first moved
    // to where the event would image, so the autofocus starts from there, and the distance it
    // moved is kept to shift the Z positions that follow.
    bool MDAManager::runAutofocus(const AcquisitionEvent& event,
                                  SequenceState& state,
                                  QString* errorMessage)
    {
        if (!m_acquisitionHardware || !m_acquisitionHardware->hasAutofocus())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No autofocus device is available");
            return false;
        }
        const QString focus = m_stageProvider ? m_stageProvider->defaultZStage() : QString();
        if (focus.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
            return false;
        }

        double before = 0.0;
        if (event.hasZ)
        {
            before = event.z + state.focusShift.value(event.positionIndex, 0.0);
            if (!moveZ(before, errorMessage))
            {
                return false;
            }
        }
        else if (!m_stageProvider->getZPosition(focus, before, errorMessage))
        {
            return false;
        }

        QString error;
        if (!m_acquisitionHardware->runAutofocus(&error))
        {
            if (errorMessage) *errorMessage = QStringLiteral("Autofocus failed: %1").arg(error);
            return false;
        }
        double after = 0.0;
        if (!m_stageProvider->getZPosition(focus, after, errorMessage))
        {
            return false;
        }

        const double moved = after - before;
        if (event.hasZ)
        {
            state.focusShift[event.positionIndex] += moved;
        }
        else
        {
            state.zReference += moved;
        }
        state.zKnown = true;
        state.z = after;
        return true;
    }

    // Routes one event to the active capture implementation
    bool MDAManager::execEvent(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage)
    {
        if (!m_cameraProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Camera provider not available");
            return false;
        }
        if (event.cameraIds.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No camera selected for acquisition event");
            return false;
        }
        return captureCameras(event, output, errorMessage);
    }

    // Captures one event across all selected camera providers
    bool MDAManager::captureCameras(const AcquisitionEvent& event,
                                    MDAOutput& output,
                                    QString* errorMessage)
    {
        const int captureTimeoutMs = static_cast<int>((std::max)(1500.0, event.exposureMs * 4.0 + 500.0));

        struct CaptureResult
        {
            QString cameraId;
            ImageFrame frame;
            bool ok{false};
            QString error;
        };

        std::vector<std::future<CaptureResult>> futures;
        futures.reserve(static_cast<size_t>(event.cameraIds.size()));
        for (const QString& cameraId : event.cameraIds)
        {
            futures.emplace_back(std::async(std::launch::async, [this, cameraId, captureTimeoutMs]()
            {
                CaptureResult result;
                result.cameraId = cameraId;
                if (!m_cameraProvider->captureEventFrame(cameraId, result.frame, captureTimeoutMs))
                {
                    result.error = QStringLiteral("Failed to capture frame from camera: %1").arg(cameraId);
                    return result;
                }
                if (!result.frame.isValid())
                {
                    result.error = QStringLiteral("Empty frame from camera: %1").arg(cameraId);
                    return result;
                }
                result.ok = true;
                return result;
            }));
        }

        for (auto& future : futures)
        {
            CaptureResult result = future.get();
            if (!result.ok)
            {
                if (errorMessage) *errorMessage = result.error;
                return false;
            }
            result.frame.cameraId = result.cameraId;
            output.frames.insert(result.cameraId, std::move(result.frame));
        }
        return true;
    }

    // Applies exposure for the next event
    bool MDAManager::setExposure(const QStringList& cameraIds,
                                 double exposureMs,
                                 QString* errorMessage)
    {
        if (!m_cameraProvider || cameraIds.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Camera provider not available");
            return false;
        }
        for (const QString& cameraId : cameraIds)
        {
            if (!m_cameraProvider->setExposure(cameraId, exposureMs))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Failed to set exposure for camera: %1")
                                        .arg(cameraId);
                }
                return false;
            }
        }
        return true;
    }

    // Moves the active XY stage to an event position
    bool MDAManager::moveXY(double x, double y, QString* errorMessage)
    {
        if (!m_stageProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Stage provider not available");
            return false;
        }
        const QString stage = m_stageProvider->defaultXYStage();
        if (stage.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No XY stage device configured");
            return false;
        }
        return m_stageProvider->setXYPosition(stage, x, y, errorMessage);
    }

    // Moves the active focus device to an event position
    bool MDAManager::moveZ(double z, QString* errorMessage)
    {
        if (!m_stageProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Stage provider not available");
            return false;
        }
        const QString focus = m_stageProvider->defaultZStage();
        if (focus.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
            return false;
        }
        return m_stageProvider->setZPosition(focus, z, errorMessage);
    }
} // namespace scopeone::core::internal
