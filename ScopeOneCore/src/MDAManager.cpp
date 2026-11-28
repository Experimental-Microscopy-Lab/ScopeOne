#include "internal/MDAManager.h"

#include "internal/HardwareRuntime.h"
#include "scopeone/ConfigSetting.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <functional>
#include <future>
#include <limits>
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

    // Connects MDA capture to the cameras
    void MDAManager::setHardwareRuntime(HardwareRuntime* hardwareRuntime)
    {
        m_hardwareRuntime = hardwareRuntime;
    }

    // Starts one immutable acquisition event sequence
    bool MDAManager::start(const QList<AcquisitionEvent>& events, MDAHook hook)
    {
        if (m_running.exchange(true))
        {
            emit sequenceError(QStringLiteral("MDA already running"));
            return false;
        }

        m_cancelRequested.store(false);
        m_threadPool.start([this, events, hook = std::move(hook)]()
        {
            // Given back in restoreInitialState, before the sequence reports its end. The preview
            // that ran before may have left the shutter open, and its stop no longer closes it
            // once the sequence holds the shutter, so the sequence starts with it closed.
            m_hardwareRuntime->setShutterHeldByAcquisition(true);
            if (m_hardwareRuntime->autoShutter())
            {
                m_hardwareRuntime->setDefaultShutterOpen(false);
            }
            runSequence(events, hook);
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
    void MDAManager::runSequence(QList<AcquisitionEvent> events, const MDAHook& hook)
    {
        QElapsedTimer timer;
        timer.start();
        const qint64 sequenceOriginMs = events.first().minimumStartTimeMs;
        // With hooks every event is taken on its own, without hardware Z or channel sequences,
        // so that each one passes both hook points
        const bool hooked = static_cast<bool>(hook);
        const std::function<bool()> canceled = [this]() { return m_cancelRequested.load(); };
        SequenceState state;
        captureInitialState(events, hooked, state);
        const auto end = [this, &state](void (MDAManager::*signal)())
        {
            restoreInitialState(state);
            m_running.store(false);
            emit (this->*signal)();
        };
        // The channel offsets move from the focus at the start, which is read here, so they are
        // checked against the limits here, before any event moves
        if (state.hasInitialZ)
        {
            const QString focus = m_hardwareRuntime->defaultZStage();
            for (const AcquisitionEvent& event : events)
            {
                QString error;
                if (!event.hasZ && event.zOffsetUm != 0.0
                    && !m_hardwareRuntime->checkLimit(focus, QStringLiteral("Z"),
                                                      state.zReference + event.zOffsetUm, &error))
                {
                    restoreInitialState(state);
                    m_running.store(false);
                    emit sequenceError(error);
                    return;
                }
            }
        }
        // With hooks the shutter closes after every event, so it is closed while a client answers,
        // and each event of a shutter block opens it again. This is whether the block goes on.
        bool shutterBlock = false;
        // What a skipped event would have done for the events after it: run the autofocus of its
        // position
        bool carryAutofocus = false;
        int carryPosition = -1;

        for (int index = 0; index < events.size();)
        {
            AcquisitionEvent event = events.at(index);
            const qint64 targetMs = event.minimumStartTimeMs - sequenceOriginMs;
            while (timer.elapsed() < targetMs)
            {
                if (m_cancelRequested.load())
                {
                    end(&MDAManager::sequenceCanceled);
                    return;
                }
                const qint64 remaining = targetMs - timer.elapsed();
                QThread::msleep(static_cast<unsigned long>((std::min<qint64>)(5, remaining)));
            }

            if (m_cancelRequested.load())
            {
                end(&MDAManager::sequenceCanceled);
                return;
            }

            if (hooked)
            {
                event.openShutterBefore = event.openShutterBefore || shutterBlock;
                shutterBlock = event.openShutterBefore && !event.closeShutterAfter;
                event.closeShutterAfter = true;
                event.autofocusBefore = event.autofocusBefore
                    || (carryAutofocus && event.positionIndex == carryPosition);
                MDAOutput request;
                request.event = event;
                const HookReply reply = hook(HookPoint::BeforeEvent, request, canceled);
                if (m_cancelRequested.load())
                {
                    end(&MDAManager::sequenceCanceled);
                    return;
                }
                if (reply.action == HookReply::Action::Stop)
                {
                    end(&MDAManager::sequenceStopped);
                    return;
                }
                if (reply.action == HookReply::Action::Skip)
                {
                    // A skipped event takes no frames and leaves no record, as the events a
                    // canceled run did not reach
                    carryAutofocus = event.autofocusBefore;
                    carryPosition = event.positionIndex;
                    ++index;
                    continue;
                }
                carryAutofocus = false;
                QString hookError;
                if (!applyHookChanges(event, reply, state, &hookError))
                {
                    restoreInitialState(state);
                    m_running.store(false);
                    emit sequenceError(hookError);
                    return;
                }
            }

            // Z slices that the hardware can time are taken in one sequence
            QList<MDAOutput> outputs;
            QString errorMessage;
            int consumed = 1;
            const int sequenceLength = hooked ? 1 : hardwareSequenceLength(events, index);
            std::vector<PropertySequence> channelSequences;
            const int channelLength = hooked || sequenceLength > 1
                ? 1
                : channelSequenceLength(events, index, channelSequences);
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
                    && captureCameras(event, output, &errorMessage);
                closeShutter(event, state);
                output.completedTimestampNs = currentTimestampNs();
                output.errorMessage = output.succeeded
                                          ? QString()
                                          : (errorMessage.isEmpty()
                                                 ? QStringLiteral("Failed to execute acquisition event")
                                                 : errorMessage);
                outputs.push_back(std::move(output));
            }

            for (const MDAOutput& output : outputs)
            {
                emit eventFinished(output);
                if (!output.succeeded)
                {
                    restoreInitialState(state);
                    m_running.store(false);
                    emit sequenceError(output.errorMessage);
                    return;
                }
            }
            if (hooked)
            {
                const HookReply reply = hook(HookPoint::AfterEvent, outputs.constFirst(), canceled);
                if (m_cancelRequested.load())
                {
                    end(&MDAManager::sequenceCanceled);
                    return;
                }
                if (reply.action == HookReply::Action::Stop)
                {
                    end(&MDAManager::sequenceStopped);
                    return;
                }
            }
            index += consumed;
        }

        end(&MDAManager::sequenceFinished);
    }

    // Takes the changes of a hook reply into the event. A position the event did not have starts
    // from where the stage is. The z of a reply is where the focus goes, so the autofocus shift of
    // the position, which setupEvent adds to the Z of an event, is taken off it here.
    bool MDAManager::applyHookChanges(AcquisitionEvent& event,
                                      const HookReply& reply,
                                      const SequenceState& state,
                                      QString* errorMessage) const
    {
        if ((reply.x || reply.y) && !event.hasXY)
        {
            if (!m_hardwareRuntime->getXYPosition(m_hardwareRuntime->defaultXYStage(), event.x, event.y))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Failed to read the stage position for the hook reply");
                }
                return false;
            }
            event.hasXY = true;
        }
        event.x = reply.x.value_or(event.x);
        event.y = reply.y.value_or(event.y);
        if (reply.z)
        {
            event.z = *reply.z - state.focusShift.value(event.positionIndex, 0.0);
            event.hasZ = true;
        }
        event.exposureMs = reply.exposureMs.value_or(event.exposureMs);
        event.changedByHook = reply.x || reply.y || reply.z || reply.exposureMs;
        return true;
    }

    // Remembers what the hardware is set to before the sequence, so it can be put back
    void MDAManager::captureInitialState(const QList<AcquisitionEvent>& events, bool hooked, SequenceState& state)
    {
        // A hook reply can move the stages of any event
        bool usesXY = hooked;
        bool usesZ = hooked;
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
        if (usesXY)
        {
            const QString stage = m_hardwareRuntime->defaultXYStage();
            state.hasInitialXY = !stage.isEmpty()
                && m_hardwareRuntime->getXYPosition(stage, state.initialX, state.initialY);
        }
        if (usesZ)
        {
            const QString focus = m_hardwareRuntime->defaultZStage();
            state.hasInitialZ = !focus.isEmpty()
                && m_hardwareRuntime->getZPosition(focus, state.initialZ);
            state.zReference = state.initialZ;
        }
        if (!channelGroup.isEmpty())
        {
            state.initialChannelGroup = channelGroup;
            state.initialChannelPreset = m_hardwareRuntime->currentConfig(channelGroup);
        }
        // Like Micro-Manager, only take over the shutter when the core controls it
        const bool wantsShutter = std::any_of(events.begin(), events.end(),
                                              [](const AcquisitionEvent& event)
                                              {
                                                  return event.openShutterBefore;
                                              });
        state.holdShutter = wantsShutter && m_hardwareRuntime->autoShutter()
            && !m_hardwareRuntime->defaultShutter().isEmpty();
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
        m_hardwareRuntime->setAutoShutter(false);
        if (!m_hardwareRuntime->setDefaultShutterOpen(true, &error))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to open the shutter: %1").arg(error);
            }
            m_hardwareRuntime->setAutoShutter(true);
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
        if (!m_hardwareRuntime->setDefaultShutterOpen(false, &error))
        {
            qWarning().noquote() << "Failed to close the shutter:" << error;
        }
        m_hardwareRuntime->setAutoShutter(true);
        state.shutterOpen = false;
    }

    // Puts the stages and the channel back where they were before the sequence
    void MDAManager::restoreInitialState(const SequenceState& state)
    {
        QString error;
        if (state.holdShutter && state.shutterOpen)
        {
            m_hardwareRuntime->setDefaultShutterOpen(false);
            m_hardwareRuntime->setAutoShutter(true);
        }
        if (state.hasInitialXY)
        {
            const QString stage = m_hardwareRuntime->defaultXYStage();
            if (!stage.isEmpty()
                && !m_hardwareRuntime->setXYPosition(stage, state.initialX, state.initialY, &error))
            {
                qWarning().noquote() << "Failed to restore the XY position:" << error;
            }
        }
        if (state.hasInitialZ)
        {
            const QString focus = m_hardwareRuntime->defaultZStage();
            if (!focus.isEmpty()
                && !m_hardwareRuntime->setZPosition(focus, state.initialZ, &error))
            {
                qWarning().noquote() << "Failed to restore the Z position:" << error;
            }
        }
        if (!state.initialChannelPreset.isEmpty()
            && state.initialChannelPreset != state.channelPreset
            && !m_hardwareRuntime->setConfig(state.initialChannelGroup,
                                                   state.initialChannelPreset,
                                                   &error))
        {
            qWarning().noquote() << "Failed to restore the channel:" << error;
        }
        m_hardwareRuntime->setShutterHeldByAcquisition(false);
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
            // The focus left the channel offset, so the next event without Z moves back to it; a
            // Z from a hook reply in a plan without Z positions stayed for the later events
            state.zOffset = std::numeric_limits<double>::quiet_NaN();
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
        QString error;
        if (!m_hardwareRuntime->setConfig(event.channelGroup, event.channelPreset, &error))
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
        if (!state.hasInitialZ)
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

    // Focuses with the autofocus device at the position of the event. An event with Z first moves
    // the focus to where it would image, and the distance the autofocus moved shifts the Z
    // positions of that position. An event without Z focuses from where the focus is.
    bool MDAManager::runAutofocus(const AcquisitionEvent& event,
                                  SequenceState& state,
                                  QString* errorMessage)
    {
        if (!m_hardwareRuntime->hasAutofocus())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No autofocus device is available");
            return false;
        }
        const QString focus = m_hardwareRuntime->defaultZStage();
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

        QString error;
        if (!m_hardwareRuntime->runAutofocus(&error))
        {
            if (errorMessage) *errorMessage = QStringLiteral("Autofocus failed: %1").arg(error);
            return false;
        }
        double after = 0.0;
        if (!m_hardwareRuntime->getZPosition(focus, after, errorMessage))
        {
            return false;
        }

        if (event.hasZ)
        {
            state.focusShift[event.positionIndex] += after - before;
        }
        else
        {
            // The event images where the autofocus ended, wherever the focus started from, and
            // the channel offsets of the later events are measured from there
            state.zReference = after - event.zOffsetUm;
            state.zOffset = event.zOffsetUm;
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
            || head.cameraIds.size() != 1
            || !m_hardwareRuntime->supportsSequence(head.cameraIds.first()))
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
        const QString focus = m_hardwareRuntime->defaultZStage();
        const long capacity = focus.isEmpty() ? 0 : m_hardwareRuntime->focusSequenceCapacity(focus);
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
        const QString focus = m_hardwareRuntime->defaultZStage();

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
                return m_hardwareRuntime->startFocusSequence(focus, positions, error);
            },
            [&]() { m_hardwareRuntime->stopFocusSequence(focus); },
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
            || head.cameraIds.size() != 1
            || !m_hardwareRuntime->supportsSequence(head.cameraIds.first()))
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
            presets.push_back(m_hardwareRuntime->configSettings(
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
            // A property the hardware cannot step by triggers, such as a role, has no capacity
            if (m_hardwareRuntime->propertySequenceCapacity(reference.device, reference.property) < length)
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
            [&](QString* error) { return m_hardwareRuntime->startPropertySequences(sequences, error); },
            [&]() { m_hardwareRuntime->stopPropertySequences(sequences); },
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
        const bool captured = m_hardwareRuntime->snapSequence(cameraId, count, frames, timeoutMs)
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
                if (!m_hardwareRuntime->snap(cameraId, result.frame, captureTimeoutMs))
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
        for (const QString& cameraId : cameraIds)
        {
            QString error;
            if (!m_hardwareRuntime->setExposure(cameraId, exposureMs, &error))
            {
                if (errorMessage)
                {
                    *errorMessage = error.isEmpty()
                        ? QStringLiteral("Failed to set exposure for camera: %1").arg(cameraId)
                        : error;
                }
                return false;
            }
        }
        return true;
    }

    // Moves the active XY stage to an event position
    bool MDAManager::moveXY(double x, double y, QString* errorMessage)
    {
        const QString stage = m_hardwareRuntime->defaultXYStage();
        if (stage.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No XY stage device configured");
            return false;
        }
        return m_hardwareRuntime->setXYPosition(stage, x, y, errorMessage);
    }

    // Moves the active focus device to an event position
    bool MDAManager::moveZ(double z, QString* errorMessage)
    {
        const QString focus = m_hardwareRuntime->defaultZStage();
        if (focus.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
            return false;
        }
        return m_hardwareRuntime->setZPosition(focus, z, errorMessage);
    }
} // namespace scopeone::core::internal
