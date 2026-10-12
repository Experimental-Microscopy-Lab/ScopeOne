#include "internal/MDAManager.h"

#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <functional>
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

        for (int index = 0; index < events.size();)
        {
            const AcquisitionEvent& event = events.at(index);
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

            // Z slices that the hardware can time are taken in one sequence
            QList<MDAOutput> outputs;
            QString errorMessage;
            int consumed = 1;
            const int sequenceLength = hardwareSequenceLength(events, index);
            std::vector<PropertySequence> channelSequences;
            const int channelLength = sequenceLength > 1 ? 1 : channelSequenceLength(events, index, channelSequences);
            bool handled = false;
            if (sequenceLength > 1 || channelLength > 1)
            {
                const int length = sequenceLength > 1 ? sequenceLength : channelLength;
                handled = sequenceLength > 1
                    ? execHardwareSequence(events, index, length, state, outputs, &errorMessage)
                    : execChannelSequence(events, index, length, channelSequences, state, outputs, &errorMessage);
                if (handled)
                {
                    consumed = length;
                }
                else
                {
                    // Fall back to taking the images one by one
                    qWarning().noquote()
                        << "Hardware sequence not used:" << errorMessage;
                    errorMessage.clear();
                    outputs.clear();
                }
            }
            if (!handled)
            {
                MDAOutput output;
                output.event = event;
                output.startedTimestampNs = currentTimestampNs();
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
                outputs.push_back(std::move(output));
            }

            bool failed = false;
            for (const MDAOutput& output : outputs)
            {
                emit eventFinished(output);
                if (!output.succeeded)
                {
                    restoreInitialState(state);
                    m_running.store(false);
                    emit sequenceError(output.errorMessage);
                    failed = true;
                    break;
                }
            }
            if (failed)
            {
                return;
            }
            index += consumed;
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

    // Finds the Z slices of one channel at one position that the hardware can take in a single
    // sequence: events that only differ in their Z position, taken by a camera that can run
    // a sequence acquisition, with a focus device that can step through its own sequence
    int MDAManager::hardwareSequenceLength(const QList<AcquisitionEvent>& events, int first) const
    {
        const AcquisitionEvent& head = events.at(first);
        if (!head.hardwareSequencing || !head.hasZ || head.autofocusBefore
            || head.cameraIds.size() != 1 || !m_cameraProvider || !m_stageProvider
            || !m_acquisitionHardware
            || !m_cameraProvider->supportsEventSequence(head.cameraIds.first()))
        {
            return 1;
        }
        int length = 1;
        while (first + length < events.size())
        {
            const AcquisitionEvent& next = events.at(first + length);
            const AcquisitionEvent& previous = events.at(first + length - 1);
            if (next.timeIndex != head.timeIndex || next.positionIndex != head.positionIndex
                || next.channelIndex != head.channelIndex || next.channelPreset != head.channelPreset
                || next.exposureMs != head.exposureMs || next.cameraIds != head.cameraIds
                || next.minimumStartTimeMs != head.minimumStartTimeMs || !next.hasZ
                || next.autofocusBefore || next.x != head.x || next.y != head.y
                || next.zIndex != previous.zIndex + 1)
            {
                break;
            }
            ++length;
        }
        if (length < 2)
        {
            return 1;
        }
        const QString focus = m_stageProvider->defaultZStage();
        const long capacity = focus.isEmpty() ? 0 : m_acquisitionHardware->focusSequenceCapacity(focus);
        return capacity >= length ? length : 1;
    }

    // Takes the Z slices of a block in one sequence. The focus device is moved to the first
    // slice, armed with the others, and the camera then takes all of them.
    bool MDAManager::execHardwareSequence(const QList<AcquisitionEvent>& events,
                                          int first,
                                          int count,
                                          SequenceState& state,
                                          QList<MDAOutput>& outputs,
                                          QString* errorMessage)
    {
        const AcquisitionEvent& head = events.at(first);
        const QString focus = m_stageProvider->defaultZStage();

        // Moves everything but the slices into place, and the focus to the first slice
        if (!setupEvent(head, state, errorMessage))
        {
            return false;
        }

        // The slices follow the autofocus result of their position, like single events
        const double shift = state.focusShift.value(head.positionIndex, 0.0);
        std::vector<double> positions;
        positions.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
        {
            positions.push_back(events.at(first + index).z + shift);
        }

        const bool captured = captureSequencedBlock(
            events, first, count, state, outputs,
            [&](QString* error)
            {
                return m_acquisitionHardware->startFocusSequence(focus, positions, error);
            },
            [&]() { m_acquisitionHardware->stopFocusSequence(focus); },
            errorMessage);
        if (!captured)
        {
            // The focus device is somewhere along the sequence now
            state.zKnown = false;
            return false;
        }
        // The focus device ends on the last position of the sequence
        state.zKnown = true;
        state.z = positions.back();
        return true;
    }

    // Finds the channels of one Z slice that the hardware can take in a single sequence:
    // events that only differ in their channel, whose presets differ in properties that can
    // step through their values on triggers
    int MDAManager::channelSequenceLength(const QList<AcquisitionEvent>& events,
                                          int first,
                                          std::vector<PropertySequence>& sequences) const
    {
        sequences.clear();
        const AcquisitionEvent& head = events.at(first);
        if (!head.hardwareSequencing || head.channelPreset.isEmpty() || head.autofocusBefore
            || head.cameraIds.size() != 1 || !m_cameraProvider || !m_acquisitionHardware
            || !m_configurationProvider
            || !m_cameraProvider->supportsEventSequence(head.cameraIds.first()))
        {
            return 1;
        }
        int length = 1;
        while (first + length < events.size())
        {
            const AcquisitionEvent& next = events.at(first + length);
            const AcquisitionEvent& previous = events.at(first + length - 1);
            if (next.timeIndex != head.timeIndex || next.positionIndex != head.positionIndex
                || next.zIndex != head.zIndex || next.z != head.z || next.hasZ != head.hasZ
                || next.zOffsetUm != head.zOffsetUm || next.exposureMs != head.exposureMs
                || next.cameraIds != head.cameraIds || next.channelGroup != head.channelGroup
                || next.channelPreset.isEmpty() || next.channelIndex <= previous.channelIndex
                || next.minimumStartTimeMs != head.minimumStartTimeMs || next.autofocusBefore
                || next.x != head.x || next.y != head.y)
            {
                break;
            }
            ++length;
        }
        if (length < 2)
        {
            return 1;
        }

        // The properties that the presets set, and what differs between the channels
        std::vector<std::vector<scopeone::core::ConfigSetting>> presets;
        for (int index = 0; index < length; ++index)
        {
            presets.push_back(m_configurationProvider->configSettings(
                head.channelGroup, events.at(first + index).channelPreset));
            if (presets.back().size() != presets.front().size())
            {
                return 1;
            }
        }
        for (const scopeone::core::ConfigSetting& reference : presets.front())
        {
            PropertySequence sequence;
            sequence.device = reference.device;
            sequence.property = reference.property;
            bool differs = false;
            for (const auto& preset : presets)
            {
                const auto found = std::find_if(preset.begin(), preset.end(),
                    [&reference](const scopeone::core::ConfigSetting& setting)
                    {
                        return setting.device == reference.device && setting.property == reference.property;
                    });
                if (found == preset.end())
                {
                    return 1;
                }
                differs = differs || found->value != reference.value;
                sequence.values.push_back(found->value);
            }
            if (!differs)
            {
                continue;
            }
            // The core and the camera cannot be stepped by triggers
            if (reference.device == QLatin1String("Core")
                || m_acquisitionHardware->propertySequenceCapacity(reference.device, reference.property) < length)
            {
                return 1;
            }
            sequences.push_back(std::move(sequence));
        }
        if (sequences.empty())
        {
            return 1;
        }
        return length;
    }

    // Takes the channels of one Z slice in one sequence. The first channel is applied as
    // usual, and the properties that differ between the channels step on the triggers.
    bool MDAManager::execChannelSequence(const QList<AcquisitionEvent>& events,
                                         int first,
                                         int count,
                                         const std::vector<PropertySequence>& sequences,
                                         SequenceState& state,
                                         QList<MDAOutput>& outputs,
                                         QString* errorMessage)
    {
        if (!setupEvent(events.at(first), state, errorMessage))
        {
            return false;
        }
        const bool captured = captureSequencedBlock(
            events, first, count, state, outputs,
            [&](QString* error) { return m_acquisitionHardware->startPropertySequences(sequences, error); },
            [&]() { m_acquisitionHardware->stopPropertySequences(sequences); },
            errorMessage);
        // The properties are left on the last channel or have wrapped around, so the next
        // event applies its preset again
        state.channelGroup.clear();
        state.channelPreset.clear();
        return captured;
    }

    // Arms the hardware, takes the images in one camera sequence and turns them into outputs
    bool MDAManager::captureSequencedBlock(const QList<AcquisitionEvent>& events,
                                           int first,
                                           int count,
                                           SequenceState& state,
                                           QList<MDAOutput>& outputs,
                                           const std::function<bool(QString*)>& arm,
                                           const std::function<void()>& disarm,
                                           QString* errorMessage)
    {
        const AcquisitionEvent& head = events.at(first);
        const AcquisitionEvent& last = events.at(first + count - 1);
        const QString cameraId = head.cameraIds.first();
        const quint64 startedNs = currentTimestampNs();

        if (!openShutter(head, state, errorMessage))
        {
            return false;
        }
        QString error;
        if (!arm(&error))
        {
            closeShutter(last, state);
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to start the hardware sequence: %1").arg(error);
            }
            return false;
        }
        std::vector<ImageFrame> frames;
        const int timeoutMs = static_cast<int>((std::max)(10000.0, head.exposureMs * count * 4.0 + 5000.0));
        const bool captured = m_cameraProvider->captureEventSequence(cameraId, count, frames, timeoutMs)
            && static_cast<int>(frames.size()) == count;
        disarm();
        closeShutter(last, state);
        if (!captured)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to capture the image sequence");
            return false;
        }

        if (!state.reportedSequence)
        {
            state.reportedSequence = true;
            qInfo().noquote() << QStringLiteral("Taking %1 images in hardware sequences").arg(count);
        }
        const quint64 completedNs = currentTimestampNs();
        for (int index = 0; index < count; ++index)
        {
            MDAOutput output;
            output.event = events.at(first + index);
            output.startedTimestampNs = startedNs;
            output.completedTimestampNs = completedNs;
            ImageFrame& frame = frames[static_cast<std::size_t>(index)];
            frame.cameraId = cameraId;
            if (!frame.isValid())
            {
                output.succeeded = false;
                output.errorMessage = QStringLiteral("Empty frame from camera: %1").arg(cameraId);
            }
            else
            {
                output.succeeded = true;
                output.frames.insert(cameraId, std::move(frame));
            }
            outputs.push_back(std::move(output));
        }
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
