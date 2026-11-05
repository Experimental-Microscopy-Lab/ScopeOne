#include "internal/DriverHostProviderProxy.h"

#include "internal/CameraRuntimeControl.h"
#include "internal/DriverHostProtocol.h"
#include "internal/HardwareStreamCodec.h"
#include "internal/SharedFrameRing.h"
#include "internal/StreamRing.h"
#include "scopeone/CameraProvider.h"
#include "scopeone/DaqProvider.h"
#include "scopeone/SignalSourceProvider.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/SharedFrame.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr int kControlReadyTimeoutMs = 15000;
        constexpr int kControlRequestTimeoutMs = 10000;

        bool responseSucceeded(const QJsonObject& response, QString* errorMessage)
        {
            if (response.value(QStringLiteral("ok")).toBool(false))
            {
                if (errorMessage) errorMessage->clear();
                return true;
            }
            if (errorMessage)
            {
                *errorMessage = response.value(QStringLiteral("error"))
                                    .toString(QStringLiteral("DriverHost request failed"));
            }
            return false;
        }

    }

    class DriverHostProviderTransport final : public QObject
    {
    public:
        using FrameHandler = std::function<void(const ImageFrame&)>;
        using PreviewHandler = std::function<void(const QString&, bool)>;

        // Where the chunks and states of the signal sources and DAQ devices go, on the transport thread
        struct StreamHandlers
        {
            SignalSourceProvider::TimeSeriesSink timeSeries;
            SignalSourceProvider::TimestampedEventSink timestampedEvents;
            SignalSourceProvider::StateSink signalState;
            DaqProvider::InputSink daqInput;
            DaqProvider::StateSink daqState;
        };

        DriverHostProviderTransport(QString providerId,
                                    QString driverPath,
                                    QString hostKey,
                                    QJsonObject options,
                                    FrameHandler frameHandler,
                                    PreviewHandler previewHandler,
                                    StreamHandlers streamHandlers)
            : m_providerId(std::move(providerId))
              , m_driverPath(std::move(driverPath))
              , m_hostKey(std::move(hostKey))
              , m_options(std::move(options))
              , m_frameHandler(std::move(frameHandler))
              , m_previewHandler(std::move(previewHandler))
              , m_streamHandlers(std::move(streamHandlers))
        {
        }

        bool initialize(HardwareProviderDescriptor& descriptor,
                        QList<HardwareDeviceDescriptor>& devices,
                        QList<SignalSourceDescriptor>& signalSources,
                        QList<DaqDeviceDescriptor>& daqDevices,
                        QString& defaultXYStage,
                        QString& defaultZStage,
                        QString* errorMessage)
        {
            m_socket = std::make_unique<QLocalSocket>();
            connect(m_socket.get(), &QLocalSocket::readyRead,
                    this, [this]() { handleReadyRead(); });
            connect(m_socket.get(), &QLocalSocket::disconnected,
                    this, [this]()
                    {
                        m_lastError = QStringLiteral("DriverHost control connection closed");
                        finishWaitingRequest();
                    });
            const QString driverHostPath = QDir(QCoreApplication::applicationDirPath())
                                               .filePath(driverhost::kExecutableFileName);
            if (!QFileInfo::exists(driverHostPath))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("DriverHost executable not found: %1")
                                        .arg(driverHostPath);
                }
                return false;
            }
            if (!QFileInfo::exists(m_driverPath))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Provider module not found: %1")
                                        .arg(m_driverPath);
                }
                return false;
            }

            m_process = std::make_unique<QProcess>();
            m_process->setProcessChannelMode(QProcess::MergedChannels);
            connect(m_process.get(), &QProcess::readyReadStandardOutput,
                    this, [this]()
                    {
                        const QString output = QString::fromUtf8(
                            m_process->readAllStandardOutput()).trimmed();
                        if (!output.isEmpty())
                        {
                            qInfo().noquote()
                                << QString("[DriverHost %1] %2").arg(m_providerId, output);
                        }
                    });
            connect(m_process.get(), &QProcess::finished,
                    this, [this](int, QProcess::ExitStatus)
                    {
                        m_lastError = QStringLiteral("DriverHost process exited");
                        for (const QString& cameraId : m_cameraIds)
                        {
                            if (m_previewHandler) m_previewHandler(cameraId, false);
                        }
                        m_runningCameras.clear();
                        endActiveStreams();
                        finishWaitingRequest();
                    });

            QStringList arguments;
            arguments << QStringLiteral("--provider") << m_providerId
                      << QStringLiteral("--driver") << m_driverPath
                      << QStringLiteral("--hostKey") << m_hostKey
                      << QStringLiteral("--launcherPid")
                      << QString::number(QCoreApplication::applicationPid());
            if (!m_options.isEmpty())
            {
                arguments << QStringLiteral("--option")
                          << QString::fromUtf8(
                                 QJsonDocument(m_options).toJson(QJsonDocument::Compact));
            }
            m_process->setProgram(driverHostPath);
            m_process->setArguments(arguments);
            m_process->start();
            if (!m_process->waitForStarted(3000))
            {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to start DriverHost");
                return false;
            }

            const QString serverName = driverhost::controlServerName(m_hostKey);
            QElapsedTimer connectTimer;
            connectTimer.start();
            while (connectTimer.elapsed() < kControlReadyTimeoutMs)
            {
                m_socket->abort();
                m_socket->connectToServer(serverName);
                if (m_socket->waitForConnected(200))
                {
                    break;
                }
                if (m_process->state() == QProcess::NotRunning)
                {
                    break;
                }
            }
            if (m_socket->state() != QLocalSocket::ConnectedState)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("DriverHost control server did not become ready");
                }
                cleanup();
                return false;
            }

            QJsonObject request;
            request.insert(driverhost::kMessageTypeField, driverhost::kCommandDescribe);
            QJsonObject response;
            if (!sendRequest(request, response, kControlRequestTimeoutMs, errorMessage)
                || !responseSucceeded(response, errorMessage))
            {
                cleanup();
                return false;
            }

            descriptor.id = response.value(driverhost::kProviderIdField).toString();
            descriptor.name = response.value(driverhost::kProviderNameField).toString();
            descriptor.version = response.value(driverhost::kProviderVersionField).toString();
            if (descriptor.id != m_providerId)
            {
                if (errorMessage) *errorMessage = QStringLiteral("Provider identity mismatch");
                cleanup();
                return false;
            }

            QSet<QString> logicalIds;
            const QJsonArray deviceArray = response.value(driverhost::kDevicesField).toArray();
            for (const QJsonValue& value : deviceArray)
            {
                const QJsonObject object = value.toObject();
                HardwareDeviceDescriptor device;
                device.logicalId = object.value(driverhost::kDeviceIdField).toString();
                device.providerId = descriptor.id;
                device.providerDeviceId =
                    object.value(driverhost::kProviderDeviceIdField).toString();
                device.hardwareId = object.value(driverhost::kHardwareIdField).toString();
                device.name = object.value(driverhost::kDeviceNameField).toString();
                device.kind = driverhost::deviceKindFromName(
                    object.value(driverhost::kDeviceKindField).toString());
                device.state = static_cast<HardwareDeviceState>(
                    object.value(driverhost::kDeviceStateField)
                        .toInt(static_cast<int>(HardwareDeviceState::Unknown)));
                device.endpoint = HardwareEndpointKind::DriverHost;
                device.properties = object.value(driverhost::kDevicePropertiesField)
                                        .toObject().toVariantMap();
                if (device.logicalId.trimmed().isEmpty()
                    || device.logicalId != device.logicalId.trimmed()
                    || logicalIds.contains(device.logicalId))
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("DriverHost returned an invalid device catalog");
                    }
                    cleanup();
                    return false;
                }
                logicalIds.insert(device.logicalId);
                devices.append(device);

                if (device.kind == HardwareDeviceKind::Camera)
                {
                    const QString shmKey =
                        object.value(driverhost::kSharedMemoryKeyField).toString();
                    if (shmKey.isEmpty())
                    {
                        if (errorMessage)
                        {
                            *errorMessage = QStringLiteral("Camera '%1' has no shared memory mapping")
                                                .arg(device.logicalId);
                        }
                        cleanup();
                        return false;
                    }
                    auto reader = std::make_shared<Reader>();
                    reader->cameraId = device.logicalId;
                    reader->shmKey = shmKey;
                    reader->shm = std::make_unique<QSharedMemory>();
                    reader->shm->setNativeKey(shmKey);
                    m_readers.insert(device.logicalId, std::move(reader));
                    m_cameraIds.append(device.logicalId);
                }
            }
            for (const QJsonValue& value : response.value(driverhost::kSignalSourcesField).toArray())
            {
                signalSources.append(hardwarestream::signalSourceDescriptorFromJson(value.toObject()));
            }
            for (const QJsonValue& value : response.value(driverhost::kDaqDevicesField).toArray())
            {
                daqDevices.append(hardwarestream::daqDeviceDescriptorFromJson(value.toObject()));
            }
            defaultXYStage = response.value(driverhost::kDefaultXYStageField).toString();
            defaultZStage = response.value(driverhost::kDefaultZStageField).toString();
            if (errorMessage) errorMessage->clear();
            return true;
        }

        // Starts a signal source or a DAQ session and attaches the ring DriverHost made for it
        bool startStream(const QString& command, const QJsonObject& config, QString* errorMessage)
        {
            QJsonObject request;
            request.insert(driverhost::kMessageTypeField, command);
            request.insert(driverhost::kConfigField, config);
            QJsonObject response;
            if (!sendRequest(request, response, kControlRequestTimeoutMs, errorMessage)
                || !responseSucceeded(response, errorMessage))
            {
                return false;
            }
            const bool signal = command == driverhost::kCommandStartSignal;
            const QString id = signal ? config.value(QStringLiteral("sourceId")).toString()
                                      : config.value(QStringLiteral("deviceId")).toString();
            auto stream = std::make_shared<StreamReader>();
            stream->streamId = response.value(driverhost::kStreamIdField).toString();
            if (!streamring::attachSharedRing(stream->shm,
                                              response.value(driverhost::kSharedMemoryKeyField).toString(),
                                              errorMessage))
            {
                stopStream(signal ? driverhost::kCommandStopSignal : driverhost::kCommandStopDaq, id, nullptr);
                return false;
            }
            stream->reader = streamring::Reader(stream->shm.data());
            m_streams.insert(stream->streamId, stream);
            (signal ? m_activeSignals : m_activeDaqDevices).insert(id);
            // Chunks written before the ring was attached have no notice of their own
            drainStream(stream->streamId);
            return true;
        }

        bool stopStream(const QString& command, const QString& id, QString* errorMessage)
        {
            const QString streamId = (command == driverhost::kCommandStopSignal ? QStringLiteral("signal:")
                                                                              : QStringLiteral("daq:"))
                                     + id;
            if (const auto stream = m_streams.value(streamId))
            {
                stream->stopRequested = true;
            }
            QJsonObject request;
            request.insert(driverhost::kMessageTypeField, command);
            request.insert(command == driverhost::kCommandStopSignal ? driverhost::kSourceIdField
                                                                     : driverhost::kDeviceIdField,
                           id);
            QJsonObject response;
            return sendRequest(request, response, kControlRequestTimeoutMs, errorMessage)
                && responseSucceeded(response, errorMessage);
        }

        bool sendRequest(QJsonObject request,
                         QJsonObject& response,
                         int timeoutMs,
                         QString* errorMessage)
        {
            if (!m_socket
                || m_socket->state() != QLocalSocket::ConnectedState
                || m_waitingLoop)
            {
                if (errorMessage)
                {
                    *errorMessage = m_waitingLoop
                                        ? QStringLiteral("Nested DriverHost request")
                                        : QStringLiteral("DriverHost is not connected");
                }
                return false;
            }

            m_waitingRequestId = m_nextRequestId++;
            m_waitingResponse = {};
            m_lastError.clear();
            request.insert(driverhost::kEnvelopeKindField, driverhost::kMessageKindRequest);
            request.insert(driverhost::kEnvelopeVersionField,
                           static_cast<int>(driverhost::kProtocolVersion));
            request.insert(driverhost::kEnvelopeRequestIdField,
                           driverhost::encodeUInt64(m_waitingRequestId));

            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            struct LoopGuard
            {
                QEventLoop*& target;
                ~LoopGuard() { target = nullptr; }
            } loopGuard{m_waitingLoop};
            m_waitingLoop = &loop;
            m_socket->write(driverhost::encodeMessage(request));
            m_socket->flush();
            timer.start((std::max)(1, timeoutMs));
            loop.exec();

            if (m_waitingResponse.isEmpty())
            {
                if (errorMessage)
                {
                    *errorMessage = m_lastError.isEmpty()
                                        ? QStringLiteral("DriverHost request timed out")
                                        : m_lastError;
                }
                m_waitingRequestId = 0;
                return false;
            }
            response = m_waitingResponse;
            m_waitingResponse = {};
            m_waitingRequestId = 0;
            if (errorMessage) errorMessage->clear();
            return true;
        }

        bool setFrameDeliveryMode(const QString& mode, QString* errorMessage)
        {
            const QString previousMode = m_deliveryMode;
            QStringList changed;
            for (const QString& cameraId : m_cameraIds)
            {
                QJsonObject request;
                request.insert(driverhost::kMessageTypeField,
                               driverhost::kCommandSetFrameDeliveryMode);
                request.insert(driverhost::kDeviceIdField, cameraId);
                request.insert(QStringLiteral("mode"), mode);
                QJsonObject response;
                if (!sendRequest(request, response, kControlRequestTimeoutMs, errorMessage)
                    || !responseSucceeded(response, errorMessage))
                {
                    for (const QString& changedCameraId : changed)
                    {
                        QJsonObject rollback;
                        rollback.insert(driverhost::kMessageTypeField,
                                        driverhost::kCommandSetFrameDeliveryMode);
                        rollback.insert(driverhost::kDeviceIdField, changedCameraId);
                        rollback.insert(QStringLiteral("mode"), previousMode);
                        QJsonObject ignored;
                        sendRequest(rollback, ignored, kControlRequestTimeoutMs, nullptr);
                    }
                    return false;
                }
                changed.append(cameraId);
            }
            m_deliveryMode = mode;
            m_readAllFrames = mode == driverhost::kFrameDeliveryModeAllFrames;
            return true;
        }

        void setFrameDeliveryPaused(bool paused)
        {
            m_frameDeliveryPaused = paused;
            if (!paused)
            {
                for (const QString& cameraId : m_runningCameras)
                {
                    deliverAvailableFrames(cameraId, false);
                }
            }
        }

        bool captureEventFrame(const QString& cameraId,
                               ImageFrame& frame,
                               int timeoutMs,
                               QString* errorMessage)
        {
            auto it = m_readers.find(cameraId);
            if (it == m_readers.end() || !ensureSharedMemory(*it.value()))
            {
                if (errorMessage) *errorMessage = QStringLiteral("Camera shared memory is unavailable");
                return false;
            }
            QList<ImageFrame> discarded;
            readLatest(*it.value(), discarded);
            const quint64 previousIndex = it.value()->lastFrameIndex;

            QJsonObject request;
            request.insert(driverhost::kMessageTypeField, driverhost::kCommandCaptureEvent);
            request.insert(driverhost::kDeviceIdField, cameraId);
            QJsonObject response;
            const int waitMs = (std::max)(1, timeoutMs);
            request.insert(QStringLiteral("timeoutMs"), waitMs);
            if (!sendRequest(request, response, waitMs + 500, errorMessage)
                || !responseSucceeded(response, errorMessage))
            {
                return false;
            }
            const quint64 targetIndex = driverhost::decodeUInt64(
                response.value(QStringLiteral("frameIndex")));

            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() <= waitMs)
            {
                QList<ImageFrame> frames;
                readLatest(*it.value(), frames);
                const ImageFrame candidate = !frames.isEmpty()
                                                 ? frames.constLast()
                                                 : it.value()->latestFrame;
                if (candidate.isValid()
                    && candidate.frameIndex > previousIndex
                    && (targetIndex == 0 || candidate.frameIndex >= targetIndex))
                {
                    frame = candidate;
                    if (errorMessage) errorMessage->clear();
                    return true;
                }
                QEventLoop loop;
                QTimer::singleShot(1, &loop, &QEventLoop::quit);
                loop.exec();
            }
            if (errorMessage) *errorMessage = QStringLiteral("Timed out waiting for captured frame");
            return false;
        }

        void cleanup()
        {
            if (m_cleanedUp) return;
            m_cleanedUp = true;
            if (m_socket && m_socket->state() == QLocalSocket::ConnectedState)
            {
                QJsonObject request;
                request.insert(driverhost::kMessageTypeField, driverhost::kCommandShutdown);
                QJsonObject response;
                sendRequest(request, response, 800, nullptr);
                m_socket->disconnectFromServer();
            }
            m_readers.clear();
            m_streams.clear();
            m_socket.reset();
            if (m_process && m_process->state() != QProcess::NotRunning)
            {
                if (!m_process->waitForFinished(1000))
                {
                    m_process->terminate();
                    if (!m_process->waitForFinished(1000))
                    {
                        m_process->kill();
                        m_process->waitForFinished(1000);
                    }
                }
            }
            m_process.reset();
        }

    private:
        struct StreamReader
        {
            QString streamId;
            QSharedMemory shm;
            streamring::Reader reader;
            bool stopRequested{false};
        };

        // A stopped stream kept its ring attached until the next start of the same source. It is
        // dropped once the source has ended, and only the ring that was stopped, as the end of an
        // earlier run can arrive after the next start.
        void releaseEndedStream(const QString& streamId, bool failed)
        {
            const auto it = m_streams.constFind(streamId);
            if (it != m_streams.cend() && (it.value()->stopRequested || failed))
            {
                m_streams.remove(streamId);
            }
        }

        void drainStream(const QString& streamId)
        {
            const auto it = m_streams.constFind(streamId);
            if (it == m_streams.cend()) return;
            const std::shared_ptr<StreamReader> stream = it.value();
            streamring::Record record;
            while (stream->reader.read(record))
            {
                if (record.droppedBefore > 0)
                {
                    qWarning().noquote() << QStringLiteral("DriverHost '%1' dropped %2 chunks of %3 because ScopeOne read too slowly")
                                                .arg(m_providerId).arg(record.droppedBefore).arg(streamId);
                }
                switch (record.type)
                {
                case streamring::RecordType::TimeSeries:
                {
                    TimeSeriesChunk chunk;
                    if (hardwarestream::decode(record.payload, chunk) && m_streamHandlers.timeSeries)
                        m_streamHandlers.timeSeries(chunk);
                    break;
                }
                case streamring::RecordType::TimestampedEvents:
                {
                    TimestampedEventChunk chunk;
                    if (hardwarestream::decode(record.payload, chunk) && m_streamHandlers.timestampedEvents)
                        m_streamHandlers.timestampedEvents(chunk);
                    break;
                }
                case streamring::RecordType::DaqInput:
                {
                    DaqInputChunk chunk;
                    if (hardwarestream::decode(record.payload, chunk) && m_streamHandlers.daqInput)
                        m_streamHandlers.daqInput(chunk);
                    break;
                }
                case streamring::RecordType::Skip:
                    break;
                }
            }
        }

        // The sources and sessions that were running when DriverHost went away end with an error
        void endActiveStreams()
        {
            const QString message = QStringLiteral("DriverHost process exited");
            for (const QString& sourceId : std::exchange(m_activeSignals, {}))
            {
                if (m_streamHandlers.signalState)
                    m_streamHandlers.signalState(sourceId, SignalSourceState::Error, message);
            }
            for (const QString& deviceId : std::exchange(m_activeDaqDevices, {}))
            {
                if (m_streamHandlers.daqState) m_streamHandlers.daqState(deviceId, DaqState::Error, message);
            }
            m_streams.clear();
        }

        struct Reader
        {
            QString cameraId;
            QString shmKey;
            std::unique_ptr<QSharedMemory> shm;
            quint64 lastFrameIndex{0};
            ImageFrame latestFrame;
        };

        void finishWaitingRequest()
        {
            if (m_waitingLoop) m_waitingLoop->quit();
        }

        void handleReadyRead()
        {
            if (!m_socket) return;
            m_readBuffer += m_socket->readAll();
            while (true)
            {
                QJsonObject message;
                QString error;
                const driverhost::DecodeResult result =
                    driverhost::tryDecodeMessage(m_readBuffer, message, &error);
                if (result == driverhost::DecodeResult::Incomplete) return;
                if (result == driverhost::DecodeResult::Error)
                {
                    m_lastError = error;
                    m_socket->abort();
                    finishWaitingRequest();
                    return;
                }
                if (message.value(driverhost::kEnvelopeVersionField).toInt(0)
                    != static_cast<int>(driverhost::kProtocolVersion))
                {
                    m_lastError = QStringLiteral("DriverHost protocol version mismatch");
                    m_socket->abort();
                    finishWaitingRequest();
                    return;
                }

                const QString kind = message.value(driverhost::kEnvelopeKindField).toString();
                if (kind == driverhost::kMessageKindResponse)
                {
                    const quint64 requestId = driverhost::decodeUInt64(
                        message.value(driverhost::kEnvelopeRequestIdField));
                    if (requestId == m_waitingRequestId)
                    {
                        m_waitingResponse = message;
                        finishWaitingRequest();
                    }
                    continue;
                }
                if (kind == driverhost::kMessageKindEvent)
                {
                    handleEvent(message);
                }
            }
        }

        void handleEvent(const QJsonObject& event)
        {
            if (event.value(driverhost::kProviderIdField).toString() != m_providerId)
            {
                return;
            }
            const QString type = event.value(driverhost::kMessageTypeField).toString();
            const QString deviceId = event.value(driverhost::kDeviceIdField).toString();
            if (type == driverhost::kEventFrameAvailable
                && !m_frameDeliveryPaused
                && m_runningCameras.contains(deviceId))
            {
                deliverAvailableFrames(deviceId, m_readAllFrames);
            }
            else if (type == driverhost::kEventPreviewState)
            {
                const bool running = event.value(QStringLiteral("running")).toBool();
                if (running) m_runningCameras.insert(deviceId);
                else m_runningCameras.remove(deviceId);
                if (m_previewHandler) m_previewHandler(deviceId, running);
            }
            else if (type == driverhost::kEventStreamAvailable)
            {
                drainStream(event.value(driverhost::kStreamIdField).toString());
            }
            else if (type == driverhost::kEventSignalState)
            {
                const QString sourceId = event.value(driverhost::kSourceIdField).toString();
                const auto state = static_cast<SignalSourceState>(event.value(driverhost::kStateField).toInt());
                // The last chunks reach the reader before the state that ends the stream
                drainStream(QStringLiteral("signal:") + sourceId);
                if (state == SignalSourceState::Idle || state == SignalSourceState::Error)
                {
                    m_activeSignals.remove(sourceId);
                    releaseEndedStream(QStringLiteral("signal:") + sourceId, state == SignalSourceState::Error);
                }
                if (m_streamHandlers.signalState)
                {
                    m_streamHandlers.signalState(sourceId, state,
                                                 event.value(driverhost::kStateMessageField).toString());
                }
            }
            else if (type == driverhost::kEventDaqState)
            {
                const auto state = static_cast<DaqState>(event.value(driverhost::kStateField).toInt());
                drainStream(QStringLiteral("daq:") + deviceId);
                if (state == DaqState::Idle || state == DaqState::Error)
                {
                    m_activeDaqDevices.remove(deviceId);
                    releaseEndedStream(QStringLiteral("daq:") + deviceId, state == DaqState::Error);
                }
                if (m_streamHandlers.daqState)
                {
                    m_streamHandlers.daqState(deviceId, state, event.value(driverhost::kStateMessageField).toString());
                }
            }
            else if (type == driverhost::kEventDriverHostError)
            {
                qWarning().noquote()
                    << QString("DriverHost '%1' error: %2")
                           .arg(m_providerId,
                                event.value(QStringLiteral("error")).toString());
            }
        }

        bool ensureSharedMemory(Reader& reader)
        {
            return sharedframe::attachRing(*reader.shm);
        }

        bool readLatest(Reader& reader, QList<ImageFrame>& frames)
        {
            if (!ensureSharedMemory(reader)
                || !sharedframe::readLatestFrame(static_cast<uchar*>(reader.shm->data()),
                                                 reader.cameraId, reader.lastFrameIndex, frames))
            {
                return false;
            }
            reader.latestFrame = frames.constLast();
            return true;
        }

        bool readAll(Reader& reader, QList<ImageFrame>& frames)
        {
            if (!ensureSharedMemory(reader)
                || !sharedframe::readNewFrames(static_cast<uchar*>(reader.shm->data()),
                                               reader.cameraId, reader.lastFrameIndex, frames))
            {
                return false;
            }
            reader.latestFrame = frames.constLast();
            return true;
        }

        void deliverAvailableFrames(const QString& cameraId, bool allFrames)
        {
            auto it = m_readers.find(cameraId);
            if (it == m_readers.end()) return;
            QList<ImageFrame> frames;
            if (allFrames) readAll(*it.value(), frames);
            else readLatest(*it.value(), frames);
            if (!m_frameHandler) return;
            for (const ImageFrame& frame : frames)
            {
                m_frameHandler(frame);
            }
        }

        QString m_providerId;
        QString m_driverPath;
        QString m_hostKey;
        QJsonObject m_options;
        FrameHandler m_frameHandler;
        PreviewHandler m_previewHandler;
        StreamHandlers m_streamHandlers;
        QHash<QString, std::shared_ptr<StreamReader>> m_streams;
        QSet<QString> m_activeSignals;
        QSet<QString> m_activeDaqDevices;
        std::unique_ptr<QProcess> m_process;
        std::unique_ptr<QLocalSocket> m_socket;
        QByteArray m_readBuffer;
        QHash<QString, std::shared_ptr<Reader>> m_readers;
        QStringList m_cameraIds;
        QSet<QString> m_runningCameras;
        QString m_deliveryMode{driverhost::kFrameDeliveryModePreviewLatest};
        QEventLoop* m_waitingLoop{nullptr};
        QJsonObject m_waitingResponse;
        QString m_lastError;
        quint64 m_nextRequestId{1};
        quint64 m_waitingRequestId{0};
        bool m_frameDeliveryPaused{false};
        bool m_readAllFrames{false};
        bool m_cleanedUp{false};
    };

    class DriverHostProviderProxy final : public QObject,
                                          public HardwareProvider,
                                          public CameraProvider,
                                          public StageProvider,
                                          public ShutterProvider,
                                          public StateProvider,
                                          public ConfigurationProvider,
                                          public SignalSourceProvider,
                                          public DaqProvider,
                                          public CameraRuntimeControl
    {
    public:
        DriverHostProviderProxy(QString providerId,
                                QString driverPath,
                                QJsonObject options)
            : m_expectedProviderId(std::move(providerId))
              , m_driverPath(std::move(driverPath))
              , m_options(std::move(options))
        {
        }

        ~DriverHostProviderProxy() override
        {
            if (m_worker && m_workerThread.isRunning())
            {
                DriverHostProviderTransport* const worker = m_worker;
                QMetaObject::invokeMethod(worker,
                                          [worker]() { worker->cleanup(); },
                                          Qt::BlockingQueuedConnection);
                m_workerThread.quit();
                m_workerThread.wait();
            }
            m_worker = nullptr;
        }

        bool initialize(QString* errorMessage)
        {
            const QString hostKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_worker = new DriverHostProviderTransport(
                m_expectedProviderId,
                m_driverPath,
                hostKey,
                m_options,
                [this](const ImageFrame& frame) { deliverFrame(frame); },
                 [this](const QString& cameraId, bool running)
                 {
                     bool anyRunning = false;
                     {
                         QMutexLocker locker(&m_stateMutex);
                         if (running) m_runningCameras.insert(cameraId);
                         else m_runningCameras.remove(cameraId);
                         anyRunning = !m_runningCameras.isEmpty();
                     }
                     PreviewStateSink sink;
                     {
                         QMutexLocker locker(&m_sinkMutex);
                         sink = m_previewStateSink;
                     }
                     if (sink) sink(anyRunning);
                 },
                 streamHandlers());
            m_worker->moveToThread(&m_workerThread);
            connect(&m_workerThread, &QThread::finished,
                    m_worker, &QObject::deleteLater);
            m_workerThread.setObjectName(
                QStringLiteral("ScopeOneProvider_%1").arg(m_expectedProviderId));
            m_workerThread.start();

            bool initialized = false;
            QString error;
            DriverHostProviderTransport* const worker = m_worker;
            QMetaObject::invokeMethod(
                worker,
                [this, worker, &initialized, &error]()
                {
                    initialized = worker->initialize(m_descriptor,
                                                     m_devices,
                                                     m_signalSources,
                                                     m_daqDevices,
                                                     m_defaultXYStage,
                                                     m_defaultZStage,
                                                     &error);
                },
                Qt::BlockingQueuedConnection);
            if (!initialized)
            {
                if (errorMessage) *errorMessage = error;
                QMetaObject::invokeMethod(worker,
                                          [worker]() { worker->cleanup(); },
                                          Qt::BlockingQueuedConnection);
                m_workerThread.quit();
                m_workerThread.wait();
                m_worker = nullptr;
                return false;
            }
            if (errorMessage) errorMessage->clear();
            return true;
        }

        HardwareProviderDescriptor descriptor() const override { return m_descriptor; }
        QList<HardwareDeviceDescriptor> devices() const override { return m_devices; }

        QList<SignalSourceDescriptor> signalSources() const override { return m_signalSources; }

        void setSignalSinks(TimeSeriesSink timeSeries,
                            TimestampedEventSink timestampedEvents,
                            SignalSourceProvider::StateSink state) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_timeSeriesSink = std::move(timeSeries);
            m_timestampedEventSink = std::move(timestampedEvents);
            m_signalStateSink = std::move(state);
        }

        bool startSignal(const SignalAcquisitionConfig& config, QString* errorMessage) override
        {
            return startStream(driverhost::kCommandStartSignal, hardwarestream::toJson(config), errorMessage);
        }

        void stopSignal(const QString& sourceId) override
        {
            stopStream(driverhost::kCommandStopSignal, sourceId);
        }

        QList<DaqDeviceDescriptor> daqDevices() const override { return m_daqDevices; }

        void setDaqSinks(InputSink input, DaqProvider::StateSink state) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_daqInputSink = std::move(input);
            m_daqStateSink = std::move(state);
        }

        bool startDaq(const DaqSessionConfig& config, QString* errorMessage) override
        {
            return startStream(driverhost::kCommandStartDaq, hardwarestream::toJson(config), errorMessage);
        }

        void stopDaq(const QString& deviceId) override
        {
            stopStream(driverhost::kCommandStopDaq, deviceId);
        }

        void setFrameSink(FrameSink sink) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_frameSink = std::move(sink);
        }

        void setPreviewStateSink(PreviewStateSink sink) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_previewStateSink = std::move(sink);
        }

        bool startPreview() override
        {
            const QStringList ids = cameraIds();
            QStringList started;
            for (const QString& cameraId : ids)
            {
                const bool wasRunning = isPreviewRunning(cameraId);
                if (!startPreviewFor(cameraId))
                {
                    for (const QString& startedId : started) stopPreviewFor(startedId);
                    return false;
                }
                if (!wasRunning) started.append(cameraId);
            }
            return !ids.isEmpty();
        }

        bool stopPreview() override
        {
            bool ok = true;
            const QStringList ids = cameraIds();
            for (const QString& cameraId : ids) ok = stopPreviewFor(cameraId) && ok;
            return ok;
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            if (!isCamera(cameraId)) return false;
            if (isPreviewRunning(cameraId)) return true;
            QJsonObject response;
            if (!request(driverhost::kCommandStartPreview, cameraId, {}, response, nullptr))
            {
                return false;
            }
            QMutexLocker locker(&m_stateMutex);
            m_runningCameras.insert(cameraId);
            return true;
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            if (!isCamera(cameraId)) return false;
            if (!isPreviewRunning(cameraId)) return true;
            QJsonObject response;
            if (!request(driverhost::kCommandStopPreview, cameraId, {}, response, nullptr))
            {
                return false;
            }
            QMutexLocker locker(&m_stateMutex);
            m_runningCameras.remove(cameraId);
            return true;
        }

        bool isPreviewRunning(const QString& cameraId) const override
        {
            QMutexLocker locker(&m_stateMutex);
            return m_runningCameras.contains(cameraId.trimmed());
        }

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const override
        {
            const QStringList targets = targetCameras(cameraIdOrAll);
            bool found = false;
            double common = 0.0;
            for (const QString& cameraId : targets)
            {
                QJsonObject response;
                if (!request(driverhost::kCommandGetExposure,
                             cameraId,
                             {},
                             response,
                             nullptr))
                {
                    return false;
                }
                const double value = response.value(QStringLiteral("exposureMs")).toDouble();
                if (found && !qFuzzyCompare(common + 1.0, value + 1.0)) return false;
                common = value;
                found = true;
            }
            if (found) exposureMs = common;
            return found;
        }

        bool setExposure(const QString& cameraIdOrAll, double exposureMs) override
        {
            const QStringList targets = targetCameras(cameraIdOrAll);
            if (targets.isEmpty() || exposureMs <= 0.0) return false;
            for (const QString& cameraId : targets)
            {
                QJsonObject fields;
                fields.insert(QStringLiteral("value"), exposureMs);
                QJsonObject response;
                if (!request(driverhost::kCommandSetExposure,
                             cameraId,
                             fields,
                             response,
                             nullptr))
                {
                    return false;
                }
            }
            return true;
        }

        QStringList listProperties(const QString& deviceId) override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandListProperties,
                         deviceId,
                         {},
                         response,
                         nullptr))
            {
                return {};
            }
            QStringList properties;
            for (const QJsonValue& value : response.value(QStringLiteral("properties")).toArray())
            {
                properties.append(value.toString());
            }
            return properties;
        }

        QString getProperty(const QString& deviceId,
                            const QString& name,
                            bool fromCache) override
        {
            QJsonObject response;
            QJsonObject fields;
            fields.insert(QStringLiteral("name"), name);
            fields.insert(QStringLiteral("fromCache"), fromCache);
            return request(driverhost::kCommandGetProperty,
                           deviceId,
                           fields,
                           response,
                           nullptr)
                       ? response.value(QStringLiteral("value")).toString()
                       : QString{};
        }

        bool setProperty(const QString& deviceId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("name"), name);
            fields.insert(QStringLiteral("value"), value);
            QJsonObject response;
            return request(driverhost::kCommandSetProperty,
                           deviceId,
                           fields,
                           response,
                           errorMessage);
        }

        QString getPropertyType(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("propertyType"))
                .toString(QStringLiteral("Unknown"));
        }

        bool isPropertyReadOnly(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("readOnly")).toBool(true);
        }

        bool isPropertyPreInit(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("preInit")).toBool(false);
        }

        QStringList getAllowedPropertyValues(const QString& deviceId,
                                             const QString& name) override
        {
            QStringList result;
            for (const QJsonValue& value :
                 propertyDetails(deviceId, name).value(QStringLiteral("allowedValues")).toArray())
            {
                result.append(value.toString());
            }
            return result;
        }

        bool hasPropertyLimits(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("hasLimits")).toBool();
        }

        double getPropertyLowerLimit(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("lowerLimit")).toDouble();
        }

        double getPropertyUpperLimit(const QString& deviceId, const QString& name) override
        {
            return propertyDetails(deviceId, name).value(QStringLiteral("upperLimit")).toDouble();
        }

        bool setROI(const QString& cameraId, int x, int y, int width, int height) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("x"), x);
            fields.insert(QStringLiteral("y"), y);
            fields.insert(QStringLiteral("width"), width);
            fields.insert(QStringLiteral("height"), height);
            QJsonObject response;
            return request(driverhost::kCommandSetRoi,
                           cameraId,
                           fields,
                           response,
                           nullptr);
        }

        bool clearROI(const QString& cameraId) override
        {
            QJsonObject response;
            return request(driverhost::kCommandClearRoi,
                           cameraId,
                           {},
                           response,
                           nullptr);
        }

        bool getROI(const QString& cameraId,
                    int& x,
                    int& y,
                    int& width,
                    int& height) override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandGetRoi,
                         cameraId,
                         {},
                         response,
                         nullptr))
            {
                return false;
            }
            x = response.value(QStringLiteral("x")).toInt();
            y = response.value(QStringLiteral("y")).toInt();
            width = response.value(QStringLiteral("width")).toInt();
            height = response.value(QStringLiteral("height")).toInt();
            return true;
        }

        bool captureEventFrame(const QString& cameraId,
                               ImageFrame& frame,
                               int timeoutMs) override
        {
            QMutexLocker requestLocker(&m_requestMutex);
            bool ok = false;
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, &ok, &frame, cameraId, timeoutMs]()
            {
                ok = worker->captureEventFrame(cameraId, frame, (std::max)(1, timeoutMs), nullptr);
            });
            return ok;
        }

        void setFrameDeliveryPaused(const QStringList&, bool paused) override
        {
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, paused]() { worker->setFrameDeliveryPaused(paused); });
        }

        bool setRecordingFrameDeliveryEnabled(const QStringList&, bool enabled) override
        {
            if (m_recordingDelivery == enabled) return true;
            const QString mode = enabled
                                     ? driverhost::kFrameDeliveryModeAllFrames
                                     : m_highRateDelivery
                                           ? driverhost::kFrameDeliveryModeLatestOnly
                                           : driverhost::kFrameDeliveryModePreviewLatest;
            if (!setDeliveryMode(mode)) return false;
            m_recordingDelivery = enabled;
            return true;
        }

        bool setHighRateFrameDeliveryEnabled(const QStringList&, bool enabled) override
        {
            if (m_highRateDelivery == enabled) return true;
            if (!m_recordingDelivery)
            {
                const QString mode = enabled
                                         ? driverhost::kFrameDeliveryModeLatestOnly
                                         : driverhost::kFrameDeliveryModePreviewLatest;
                if (!setDeliveryMode(mode)) return false;
            }
            m_highRateDelivery = enabled;
            return true;
        }

        bool isProcessingFrameTokenCurrent(const QString&, quint64) override { return false; }
        void finishProcessingFrame(const QString&, quint64) override {}

        QString defaultXYStage() const override { return m_defaultXYStage; }
        QString defaultZStage() const override { return m_defaultZStage; }

        bool getXYPosition(const QString& deviceId,
                           double& x,
                           double& y,
                           QString* errorMessage) const override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandGetXYPosition,
                         deviceId,
                         {},
                         response,
                         errorMessage))
            {
                return false;
            }
            x = response.value(QStringLiteral("x")).toDouble();
            y = response.value(QStringLiteral("y")).toDouble();
            return true;
        }

        bool getZPosition(const QString& deviceId,
                          double& z,
                          QString* errorMessage) const override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandGetZPosition,
                         deviceId,
                         {},
                         response,
                         errorMessage))
            {
                return false;
            }
            z = response.value(QStringLiteral("z")).toDouble();
            return true;
        }

        bool setRelativeXYPosition(const QString& deviceId,
                                   double dx,
                                   double dy,
                                   QString* errorMessage) override
        {
            return stageWrite(driverhost::kCommandSetRelativeXYPosition,
                              deviceId,
                              dx,
                              dy,
                              errorMessage);
        }

        bool setRelativeZPosition(const QString& deviceId,
                                  double dz,
                                  QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("z"), dz);
            QJsonObject response;
            return request(driverhost::kCommandSetRelativeZPosition,
                           deviceId,
                           fields,
                           response,
                           errorMessage);
        }

        bool setXYPosition(const QString& deviceId,
                           double x,
                           double y,
                           QString* errorMessage) override
        {
            return stageWrite(driverhost::kCommandSetXYPosition,
                              deviceId,
                              x,
                              y,
                              errorMessage);
        }

        bool setZPosition(const QString& deviceId,
                          double z,
                          QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("z"), z);
            QJsonObject response;
            return request(driverhost::kCommandSetZPosition,
                           deviceId,
                           fields,
                           response,
                           errorMessage);
        }

        bool isShutterOpen(const QString& deviceId,
                           bool& open,
                           QString* errorMessage) const override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandGetShutterOpen,
                         deviceId,
                         {},
                         response,
                         errorMessage))
            {
                return false;
            }
            open = response.value(QStringLiteral("open")).toBool();
            return true;
        }

        bool setShutterOpen(const QString& deviceId,
                            bool open,
                            QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("open"), open);
            QJsonObject response;
            return request(driverhost::kCommandSetShutterOpen,
                           deviceId,
                           fields,
                           response,
                           errorMessage);
        }

        bool getState(const QString& deviceId,
                      long& state,
                      QString* errorMessage) const override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandGetState,
                         deviceId,
                         {},
                         response,
                         errorMessage))
            {
                return false;
            }
            state = static_cast<long>(response.value(QStringLiteral("state")).toDouble());
            return true;
        }

        bool setState(const QString& deviceId,
                      long state,
                      QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("state"), static_cast<double>(state));
            QJsonObject response;
            return request(driverhost::kCommandSetState,
                           deviceId,
                           fields,
                           response,
                           errorMessage);
        }

        QString stateLabel(const QString& deviceId, long state) const override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("state"), static_cast<double>(state));
            QJsonObject response;
            return request(driverhost::kCommandGetStateLabel,
                           deviceId,
                           fields,
                           response,
                           nullptr)
                       ? response.value(QStringLiteral("label")).toString()
                       : QString{};
        }

        QStringList availableConfigGroups() const override
        {
            QJsonObject response;
            if (!request(driverhost::kCommandListConfigGroups,
                         {},
                         {},
                         response,
                         nullptr))
            {
                return {};
            }
            return jsonStringList(response.value(QStringLiteral("groups")).toArray());
        }

        QStringList availableConfigs(const QString& groupName) const override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("group"), groupName);
            QJsonObject response;
            return request(driverhost::kCommandListConfigs,
                           {},
                           fields,
                           response,
                           nullptr)
                       ? jsonStringList(response.value(QStringLiteral("configs")).toArray())
                       : QStringList{};
        }

        QString currentConfig(const QString& groupName) const override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("group"), groupName);
            QJsonObject response;
            return request(driverhost::kCommandGetCurrentConfig,
                           {},
                           fields,
                           response,
                           nullptr)
                       ? response.value(QStringLiteral("config")).toString()
                       : QString{};
        }

        bool setConfig(const QString& groupName,
                       const QString& configName,
                       QString* errorMessage) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("group"), groupName);
            fields.insert(QStringLiteral("config"), configName);
            QJsonObject response;
            return request(driverhost::kCommandSetConfig,
                           {},
                           fields,
                           response,
                           errorMessage);
        }

    private:
        void deliverFrame(const ImageFrame& frame)
        {
            FrameSink sink;
            {
                QMutexLocker locker(&m_sinkMutex);
                sink = m_frameSink;
            }
            if (sink) sink(frame);
        }

        QStringList cameraIds() const
        {
            QStringList result;
            for (const HardwareDeviceDescriptor& device : m_devices)
            {
                if (device.kind == HardwareDeviceKind::Camera) result.append(device.logicalId);
            }
            return result;
        }

        bool isCamera(const QString& cameraId) const
        {
            const QString normalized = cameraId.trimmed();
            return std::any_of(m_devices.cbegin(), m_devices.cend(),
                               [&normalized](const HardwareDeviceDescriptor& device)
                               {
                                   return device.kind == HardwareDeviceKind::Camera
                                       && device.logicalId == normalized;
                               });
        }

        QStringList targetCameras(const QString& cameraIdOrAll) const
        {
            const QString target = cameraIdOrAll.trimmed();
            if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
            {
                return cameraIds();
            }
            return isCamera(target) ? QStringList{target} : QStringList{};
        }

        bool request(const QString& type,
                     const QString& deviceId,
                     const QJsonObject& fields,
                     QJsonObject& response,
                     QString* errorMessage) const
        {
            QMutexLocker requestLocker(&m_requestMutex);
            QJsonObject request = fields;
            request.insert(driverhost::kMessageTypeField, type);
            if (!deviceId.isEmpty())
            {
                request.insert(driverhost::kDeviceIdField, deviceId.trimmed());
            }
            bool transported = false;
            QString transportError;
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, request, &response, &transported, &transportError]()
            {
                transported = worker->sendRequest(request,
                                                  response,
                                                  kControlRequestTimeoutMs,
                                                  &transportError);
            });
            if (!transported)
            {
                if (errorMessage) *errorMessage = transportError;
                return false;
            }
            return responseSucceeded(response, errorMessage);
        }

        QJsonObject propertyDetails(const QString& deviceId, const QString& name)
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("name"), name);
            fields.insert(QStringLiteral("fromCache"), true);
            QJsonObject response;
            return request(driverhost::kCommandGetProperty,
                           deviceId,
                           fields,
                           response,
                           nullptr)
                       ? response
                       : QJsonObject{};
        }

        bool stageWrite(const QString& command,
                        const QString& deviceId,
                        double x,
                        double y,
                        QString* errorMessage)
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("x"), x);
            fields.insert(QStringLiteral("y"), y);
            QJsonObject response;
            return request(command, deviceId, fields, response, errorMessage);
        }

        bool startStream(const QString& command, const QJsonObject& config, QString* errorMessage)
        {
            QMutexLocker requestLocker(&m_requestMutex);
            bool ok = false;
            QString error;
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, &command, &config, &ok, &error]() { ok = worker->startStream(command, config, &error); });
            if (errorMessage) *errorMessage = error;
            return ok;
        }

        void stopStream(const QString& command, const QString& id)
        {
            QMutexLocker requestLocker(&m_requestMutex);
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, &command, &id]() { worker->stopStream(command, id, nullptr); });
        }

        // The transport calls these on its thread; each hands the chunk or state to the sink Core set
        DriverHostProviderTransport::StreamHandlers streamHandlers()
        {
            DriverHostProviderTransport::StreamHandlers handlers;
            handlers.timeSeries = [this](const TimeSeriesChunk& chunk)
            {
                TimeSeriesSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_timeSeriesSink;
                }
                if (sink) sink(chunk);
            };
            handlers.timestampedEvents = [this](const TimestampedEventChunk& chunk)
            {
                TimestampedEventSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_timestampedEventSink;
                }
                if (sink) sink(chunk);
            };
            handlers.signalState = [this](const QString& sourceId, SignalSourceState state, const QString& message)
            {
                SignalSourceProvider::StateSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_signalStateSink;
                }
                if (sink) sink(sourceId, state, message);
            };
            handlers.daqInput = [this](const DaqInputChunk& chunk)
            {
                InputSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_daqInputSink;
                }
                if (sink) sink(chunk);
            };
            handlers.daqState = [this](const QString& deviceId, DaqState state, const QString& message)
            {
                DaqProvider::StateSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_daqStateSink;
                }
                if (sink) sink(deviceId, state, message);
            };
            return handlers;
        }

        bool setDeliveryMode(const QString& mode)
        {
            QMutexLocker requestLocker(&m_requestMutex);
            bool ok = false;
            DriverHostProviderTransport* const worker = m_worker;
            runOnWorker([worker, mode, &ok]() { ok = worker->setFrameDeliveryMode(mode, nullptr); });
            return ok;
        }

        // Runs a call on the transport thread and waits for it. Frame and preview callbacks
        // already run there, so a call from them runs directly instead of waiting on itself.
        template <typename Function>
        void runOnWorker(Function&& function) const
        {
            if (QThread::currentThread() == m_worker->thread())
            {
                function();
                return;
            }
            QMetaObject::invokeMethod(m_worker, std::forward<Function>(function), Qt::BlockingQueuedConnection);
        }

        static QStringList jsonStringList(const QJsonArray& array)
        {
            QStringList result;
            for (const QJsonValue& value : array) result.append(value.toString());
            return result;
        }

        QString m_expectedProviderId;
        QString m_driverPath;
        QJsonObject m_options;
        HardwareProviderDescriptor m_descriptor;
        QList<HardwareDeviceDescriptor> m_devices;
        QString m_defaultXYStage;
        QString m_defaultZStage;
        QThread m_workerThread;
        DriverHostProviderTransport* m_worker{nullptr};
        mutable QMutex m_requestMutex;
        mutable QMutex m_sinkMutex;
        FrameSink m_frameSink;
        PreviewStateSink m_previewStateSink;
        TimeSeriesSink m_timeSeriesSink;
        TimestampedEventSink m_timestampedEventSink;
        SignalSourceProvider::StateSink m_signalStateSink;
        InputSink m_daqInputSink;
        DaqProvider::StateSink m_daqStateSink;
        QList<SignalSourceDescriptor> m_signalSources;
        QList<DaqDeviceDescriptor> m_daqDevices;
        mutable QMutex m_stateMutex;
        QSet<QString> m_runningCameras;
        bool m_recordingDelivery{false};
        bool m_highRateDelivery{false};
    };

    HardwareProviderPtr createDriverHostProviderProxy(const QString& providerId,
                                                      const QString& driverPath,
                                                      const QJsonObject& options,
                                                      QString* errorMessage)
    {
        const QString normalizedProviderId = providerId.trimmed();
        const QString normalizedDriverPath = QFileInfo(driverPath).absoluteFilePath();
        if (normalizedProviderId.isEmpty() || driverPath.trimmed().isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Provider ID and module path are required");
            return {};
        }
        auto proxy = std::make_shared<DriverHostProviderProxy>(normalizedProviderId,
                                                               normalizedDriverPath,
                                                               options);
        if (!proxy->initialize(errorMessage)) return {};
        return proxy;
    }
}
