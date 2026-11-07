#include "internal/DriverHostClient.h"

#include "internal/CameraFrameDelivery.h"
#include "internal/DriverHostProtocol.h"
#include "internal/HardwareStreamCodec.h"
#include "internal/SharedFrameRing.h"
#include "internal/StreamRing.h"
#include "scopeone/Camera.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/SignalSource.h"
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

        const QString& frameDeliveryModeName(FrameDeliveryMode mode)
        {
            switch (mode)
            {
            case FrameDeliveryMode::LatestOnly:
                return driverhost::kFrameDeliveryModeLatestOnly;
            case FrameDeliveryMode::AllFrames:
                return driverhost::kFrameDeliveryModeAllFrames;
            case FrameDeliveryMode::PreviewLatest:
                break;
            }
            return driverhost::kFrameDeliveryModePreviewLatest;
        }
    }

    // Talks to one DriverHost on a thread of its own and reads the frames and streams it writes
    class DriverHostTransport final : public QObject
    {
    public:
        using PreviewHandler = std::function<void(const QString&, bool)>;

        // Where the chunks and states of the signal sources and DAQ devices go, on the transport thread
        struct StreamHandlers
        {
            SignalSource::TimeSeriesSink timeSeries;
            SignalSource::TimestampedEventSink timestampedEvents;
            SignalSource::StateSink signalState;
            Daq::InputSink daqInput;
            Daq::StateSink daqState;
        };

        DriverHostTransport(QString providerId,
                            QStringList arguments,
                            QString hostKey,
                            PreviewHandler previewHandler,
                            StreamHandlers streamHandlers)
            : m_providerId(std::move(providerId))
              , m_arguments(std::move(arguments))
              , m_hostKey(std::move(hostKey))
              , m_previewHandler(std::move(previewHandler))
              , m_streamHandlers(std::move(streamHandlers))
        {
        }

        void setFrameDelivery(CameraFrameDelivery* delivery)
        {
            m_delivery = delivery;
        }

        bool initialize(HardwareProviderDescriptor& descriptor,
                        QList<HardwareDeviceDescriptor>& devices,
                        QList<SignalSourceDescriptor>& signalSources,
                        QList<DaqDeviceDescriptor>& daqDevices,
                        QJsonObject& description,
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

            m_process = std::make_unique<QProcess>();
            m_process->setProcessChannelMode(QProcess::MergedChannels);
            connect(m_process.get(), &QProcess::readyReadStandardOutput,
                    this, [this]()
                    {
                        const QString output = QString::fromUtf8(
                            m_process->readAllStandardOutput()).trimmed();
                        if (!output.isEmpty())
                        {
                            m_lastHostLine = output.split(QLatin1Char('\n')).last();
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
                        if (!m_runningCameras.isEmpty() && m_delivery)
                        {
                            m_delivery->fail(QStringLiteral("DriverHost of '%1' exited").arg(m_providerId));
                        }
                        m_runningCameras.clear();
                        endActiveStreams();
                        finishWaitingRequest();
                    });

            QStringList arguments = m_arguments;
            arguments << QStringLiteral("--provider") << m_providerId
                      << QStringLiteral("--hostKey") << m_hostKey
                      << QStringLiteral("--launcherPid")
                      << QString::number(QCoreApplication::applicationPid());
            // Qt sends the messages of a process without a console, as the host started by the
            // Windows GUI is, to the debugger, so the host's reasons never reached the log there
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
            m_process->setProcessEnvironment(environment);
            m_process->setProgram(driverHostPath);
            m_process->setArguments(arguments);
            m_process->start();
            if (!m_process->waitForStarted(3000))
            {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to start DriverHost");
                return false;
            }

            const QString serverName = driverhost::controlServerName(m_hostKey);
            // This runs on a worker thread without an event loop, so the exit is polled for here
            bool exited = false;
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
                if (m_process->waitForFinished(0))
                {
                    exited = true;
                    const QString rest = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
                    if (!rest.isEmpty())
                    {
                        // The last output was read here and not logged, so a host that exited
                        // at start left nothing of its reason in the log
                        m_lastHostLine = rest.split(QLatin1Char('\n')).last().trimmed();
                        qInfo().noquote() << QString("[DriverHost %1] %2").arg(m_providerId, rest);
                    }
                    break;
                }
            }
            if (m_socket->state() != QLocalSocket::ConnectedState)
            {
                if (errorMessage)
                {
                    // A host that exits at start names its reason as its last line
                    const QString reasonPrefix =
                        QStringLiteral("DriverHost init failed for '%1': ").arg(m_hostKey);
                    const qsizetype reasonStart = m_lastHostLine.indexOf(reasonPrefix);
                    if (exited && reasonStart >= 0)
                    {
                        *errorMessage = m_lastHostLine.mid(reasonStart + reasonPrefix.size());
                    }
                    else if (exited)
                    {
                        *errorMessage = QStringLiteral("DriverHost exited with code 0x%1 before it was ready")
                                            .arg(static_cast<quint32>(m_process->exitCode()), 8, 16, QLatin1Char('0'));
                    }
                    else
                    {
                        *errorMessage = QStringLiteral("DriverHost control server did not become ready");
                    }
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

            m_rename = m_arguments.contains(QStringLiteral("--driver"));
            QSet<QString> logicalIds;
            const QJsonArray deviceArray = response.value(driverhost::kDevicesField).toArray();
            const QJsonArray sourceArray = response.value(driverhost::kSignalSourcesField).toArray();
            const QJsonArray daqArray = response.value(driverhost::kDaqDevicesField).toArray();
            int deviceIndex = 0;
            for (const QJsonValue& value : deviceArray)
            {
                const QJsonObject object = value.toObject();
                HardwareDeviceDescriptor device;
                device.logicalId = m_devices.add(
                    object.value(driverhost::kDeviceIdField).toString(),
                    logicalName(deviceArray.size(), deviceIndex++));
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
                    reader->shm = std::make_unique<QSharedMemory>(driverhost::sharedMemoryNativeKey(shmKey));
                    m_readers.insert(device.logicalId, std::move(reader));
                    m_cameraIds.append(device.logicalId);
                }
            }
            int sourceIndex = 0;
            for (const QJsonValue& value : sourceArray)
            {
                SignalSourceDescriptor source = hardwarestream::signalSourceDescriptorFromJson(value.toObject());
                source.id = m_sources.add(source.id, logicalName(sourceArray.size(), sourceIndex++));
                signalSources.append(source);
            }
            int daqIndex = 0;
            for (const QJsonValue& value : daqArray)
            {
                DaqDeviceDescriptor daq = hardwarestream::daqDeviceDescriptorFromJson(value.toObject());
                daq.id = m_daqDevices.add(daq.id, logicalName(daqArray.size(), daqIndex++));
                daqDevices.append(daq);
            }
            // Everything downstream of the description reads logical ids. The cameras kept the ids
            // of the driver here, so the event sequence and pixel size of a renamed camera were
            // looked up under the wrong id.
            QJsonArray logicalDevices;
            for (const QJsonValue& value : deviceArray)
            {
                QJsonObject object = value.toObject();
                object.insert(driverhost::kDeviceIdField,
                              m_devices.toLogical(object.value(driverhost::kDeviceIdField).toString()));
                logicalDevices.append(object);
            }
            response.insert(driverhost::kDevicesField, logicalDevices);
            for (const QString& field : {driverhost::kDefaultXYStageField, driverhost::kDefaultZStageField,
                                         driverhost::kDefaultShutterField, driverhost::kDefaultAutofocusField})
            {
                response.insert(field, m_devices.toLogical(response.value(field).toString()));
            }
            // The camera rings are attached now, so DriverHost can remove their names
            QStringList keys;
            for (const auto& reader : std::as_const(m_readers))
            {
                if (!ensureSharedMemory(*reader))
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("Cannot attach the frame ring of '%1'").arg(reader->cameraId);
                    }
                    cleanup();
                    return false;
                }
                keys.append(reader->shmKey);
            }
            releaseSharedMemory(keys);
            description = response;
            if (errorMessage) errorMessage->clear();
            return true;
        }

        void releaseSharedMemory(const QStringList& keys)
        {
            if (keys.isEmpty())
            {
                return;
            }
            QJsonObject request{{driverhost::kMessageTypeField, driverhost::kCommandReleaseSharedMemory},
                                {driverhost::kKeysField, QJsonArray::fromStringList(keys)}};
            QJsonObject response;
            sendRequest(request, response, kControlRequestTimeoutMs, nullptr);
        }

        // The images wait in DriverHost and come over a part at a time, as the frame ring holds
        // fewer images than a sequence can have
        bool captureEventSequence(const QString& cameraId,
                                  int count,
                                  std::vector<ImageFrame>& frames,
                                  int timeoutMs)
        {
            constexpr int kImagesPerPart = 16;
            auto it = m_readers.find(cameraId);
            if (it == m_readers.end() || count < 1 || !ensureSharedMemory(*it.value()))
            {
                return false;
            }
            QList<ImageFrame> discarded;
            readLatest(*it.value(), discarded);
            QJsonObject request{{driverhost::kMessageTypeField, driverhost::kCommandCaptureEventSequence},
                                {driverhost::kDeviceIdField, cameraId},
                                {QStringLiteral("count"), count},
                                {QStringLiteral("timeoutMs"), timeoutMs}};
            QJsonObject response;
            if (!sendRequest(request, response, timeoutMs + kControlRequestTimeoutMs, nullptr)
                || !responseSucceeded(response, nullptr))
            {
                return false;
            }
            frames.clear();
            frames.reserve(static_cast<std::size_t>(count));
            for (int first = 0; first < count; first += kImagesPerPart)
            {
                const int partCount = (std::min)(kImagesPerPart, count - first);
                QJsonObject part{{driverhost::kMessageTypeField, driverhost::kCommandSendEventFrames},
                                 {driverhost::kDeviceIdField, cameraId},
                                 {QStringLiteral("first"), first},
                                 {QStringLiteral("count"), partCount}};
                QList<ImageFrame> partFrames;
                if (!sendRequest(part, response, kControlRequestTimeoutMs, nullptr)
                    || !responseSucceeded(response, nullptr)
                    || !readAll(*it.value(), partFrames)
                    || partFrames.size() != partCount)
                {
                    return false;
                }
                frames.insert(frames.end(), partFrames.cbegin(), partFrames.cend());
            }
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
            stream->streamId = logicalStreamId(response.value(driverhost::kStreamIdField).toString());
            const QString shmKey = response.value(driverhost::kSharedMemoryKeyField).toString();
            if (!streamring::attachSharedRing(stream->shm, driverhost::sharedMemoryNativeKey(shmKey), errorMessage))
            {
                stopStream(signal ? driverhost::kCommandStopSignal : driverhost::kCommandStopDaq, id, nullptr);
                return false;
            }
            releaseSharedMemory({shmKey});
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

            translateRequest(request);
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

        // Only the cameras named change, as the others of the DriverHost may keep their live view
        bool setFrameDeliveryMode(const QStringList& cameraIds, FrameDeliveryMode mode)
        {
            const auto send = [this](const QString& cameraId, FrameDeliveryMode deliveryMode)
            {
                QJsonObject request;
                request.insert(driverhost::kMessageTypeField, driverhost::kCommandSetFrameDeliveryMode);
                request.insert(driverhost::kDeviceIdField, cameraId);
                request.insert(QStringLiteral("mode"), frameDeliveryModeName(deliveryMode));
                QJsonObject response;
                return sendRequest(request, response, kControlRequestTimeoutMs, nullptr)
                    && responseSucceeded(response, nullptr);
            };
            QStringList changed;
            for (const QString& cameraId : cameraIds)
            {
                if (!send(cameraId, mode))
                {
                    for (const QString& changedCameraId : changed)
                    {
                        send(changedCameraId, m_deliveryModes.value(changedCameraId));
                    }
                    return false;
                }
                changed.append(cameraId);
            }
            for (const QString& cameraId : cameraIds)
            {
                m_deliveryModes.insert(cameraId, mode);
                const auto reader = m_readers.value(cameraId);
                if (!reader)
                {
                    continue;
                }
                // Frames written before the switch belong to the live view, not to the recording
                if (mode == FrameDeliveryMode::AllFrames)
                {
                    QList<ImageFrame> discarded;
                    readLatest(*reader, discarded);
                }
                reader->resync = true;
            }
            return true;
        }

        // The DriverHost stops writing frames too, and a Micro-Manager camera that an event
        // capture stopped starts its live acquisition again when the pause ends
        void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
        {
            for (const QString& cameraId : cameraIds)
            {
                if (paused) m_pausedCameras.insert(cameraId);
                else m_pausedCameras.remove(cameraId);
                QJsonObject request;
                request.insert(driverhost::kMessageTypeField, driverhost::kCommandSetFrameDeliveryPaused);
                request.insert(driverhost::kDeviceIdField, cameraId);
                request.insert(QStringLiteral("paused"), paused);
                QJsonObject response;
                sendRequest(request, response, kControlRequestTimeoutMs, nullptr);
                if (!paused && m_runningCameras.contains(cameraId))
                {
                    deliverAvailableFrames(cameraId);
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
                    {
                        chunk.sourceId = m_sources.toLogical(chunk.sourceId);
                        m_streamHandlers.timeSeries(chunk);
                    }
                    break;
                }
                case streamring::RecordType::TimestampedEvents:
                {
                    TimestampedEventChunk chunk;
                    if (hardwarestream::decode(record.payload, chunk) && m_streamHandlers.timestampedEvents)
                    {
                        chunk.sourceId = m_sources.toLogical(chunk.sourceId);
                        m_streamHandlers.timestampedEvents(chunk);
                    }
                    break;
                }
                case streamring::RecordType::DaqInput:
                {
                    DaqInputChunk chunk;
                    if (hardwarestream::decode(record.payload, chunk) && m_streamHandlers.daqInput)
                    {
                        chunk.deviceId = m_daqDevices.toLogical(chunk.deviceId);
                        m_streamHandlers.daqInput(chunk);
                    }
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
            // The frames a camera skipped before its delivery mode changed are not counted again
            bool resync{false};
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
                    handleEvent(translateEvent(message));
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
                && !m_pausedCameras.contains(deviceId)
                && m_runningCameras.contains(deviceId))
            {
                deliverAvailableFrames(deviceId);
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
                const QString error = QStringLiteral("DriverHost '%1' error: %2")
                                          .arg(m_providerId, event.value(QStringLiteral("error")).toString());
                qWarning().noquote() << error;
                if (m_delivery)
                {
                    m_delivery->fail(error);
                }
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

        // A frame that is skipped stays counted, as the frame index of the next one tells how
        // many the camera took in between
        void deliverAvailableFrames(const QString& cameraId)
        {
            auto it = m_readers.find(cameraId);
            CameraFrameDelivery* const delivery = m_delivery;
            if (it == m_readers.end() || !delivery)
            {
                return;
            }
            const bool allFrames = m_deliveryModes.value(cameraId) == FrameDeliveryMode::AllFrames;
            if (!allFrames && !delivery->wantsLatestFrame(cameraId))
            {
                return;
            }
            Reader& reader = *it.value();
            const quint64 previousFrameIndex = reader.lastFrameIndex;
            QList<ImageFrame> frames;
            const bool read = allFrames ? readAll(reader, frames) : readLatest(reader, frames);
            if (read)
            {
                const quint64 acquiredFrameCount = std::exchange(reader.resync, false)
                                                       ? static_cast<quint64>(frames.size())
                                                       : reader.lastFrameIndex - previousFrameIndex;
                delivery->deliverFrames(frames, acquiredFrameCount);
            }
        }

        // The ids of a ScopeOne device as its driver names them and as ScopeOne knows them. The
        // driver and DriverHost know no labels, so the ids are translated here, in the requests
        // to DriverHost and in what comes back.
        struct IdMap
        {
            QHash<QString, QString> toDriverId;
            QHash<QString, QString> toLogicalId;

            // An empty logical id keeps the id of the driver
            QString add(const QString& driverId, const QString& name)
            {
                const QString logicalId = name.isEmpty() ? driverId : name;
                toDriverId.insert(logicalId, driverId);
                if (!driverId.isEmpty())
                {
                    toLogicalId.insert(driverId, logicalId);
                }
                return logicalId;
            }
            QString toDriver(const QString& id) const { return toDriverId.value(id, id); }
            QString toLogical(const QString& id) const { return toLogicalId.value(id, id); }
        };

        // The label for a single device, and the label with the number of the device for several
        QString logicalName(qsizetype count, int index) const
        {
            if (!m_rename)
            {
                return {};
            }
            return count == 1 ? m_providerId : QStringLiteral("%1.%2").arg(m_providerId).arg(index + 1);
        }

        QString logicalStreamId(const QString& streamId) const
        {
            const qsizetype colon = streamId.indexOf(QLatin1Char(':'));
            const QString kind = streamId.left(colon + 1);
            const QString id = streamId.mid(colon + 1);
            if (kind == QStringLiteral("signal:")) return kind + m_sources.toLogical(id);
            if (kind == QStringLiteral("daq:")) return kind + m_daqDevices.toLogical(id);
            return streamId;
        }

        void translateRequest(QJsonObject& request) const
        {
            const bool daq = request.value(driverhost::kMessageTypeField).toString()
                             == driverhost::kCommandStopDaq;
            if (request.contains(driverhost::kDeviceIdField))
            {
                const QString id = request.value(driverhost::kDeviceIdField).toString();
                request.insert(driverhost::kDeviceIdField,
                               daq ? m_daqDevices.toDriver(id) : m_devices.toDriver(id));
            }
            if (request.contains(driverhost::kSourceIdField))
            {
                request.insert(driverhost::kSourceIdField,
                               m_sources.toDriver(request.value(driverhost::kSourceIdField).toString()));
            }
            QJsonObject config = request.value(driverhost::kConfigField).toObject();
            if (config.contains(QStringLiteral("sourceId")))
            {
                config.insert(QStringLiteral("sourceId"),
                              m_sources.toDriver(config.value(QStringLiteral("sourceId")).toString()));
            }
            if (config.contains(QStringLiteral("deviceId")))
            {
                config.insert(QStringLiteral("deviceId"),
                              m_daqDevices.toDriver(config.value(QStringLiteral("deviceId")).toString()));
            }
            if (request.contains(driverhost::kConfigField))
            {
                request.insert(driverhost::kConfigField, config);
            }
        }

        QJsonObject translateEvent(QJsonObject event) const
        {
            const QString type = event.value(driverhost::kMessageTypeField).toString();
            if (event.contains(driverhost::kDeviceIdField))
            {
                const QString id = event.value(driverhost::kDeviceIdField).toString();
                event.insert(driverhost::kDeviceIdField,
                             type == driverhost::kEventDaqState ? m_daqDevices.toLogical(id)
                                                                : m_devices.toLogical(id));
            }
            if (event.contains(driverhost::kSourceIdField))
            {
                event.insert(driverhost::kSourceIdField,
                             m_sources.toLogical(event.value(driverhost::kSourceIdField).toString()));
            }
            if (event.contains(driverhost::kStreamIdField))
            {
                event.insert(driverhost::kStreamIdField,
                             logicalStreamId(event.value(driverhost::kStreamIdField).toString()));
            }
            return event;
        }

        QString m_providerId;
        QStringList m_arguments;
        QString m_hostKey;
        bool m_rename{false};
        IdMap m_devices;
        IdMap m_sources;
        IdMap m_daqDevices;
        // Set and read on the transport thread only
        CameraFrameDelivery* m_delivery{nullptr};
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
        QHash<QString, FrameDeliveryMode> m_deliveryModes;
        QSet<QString> m_pausedCameras;
        QEventLoop* m_waitingLoop{nullptr};
        QJsonObject m_waitingResponse;
        QString m_lastError;
        // The last line the DriverHost printed, which is its reason when it exits at start
        QString m_lastHostLine;
        quint64 m_nextRequestId{1};
        quint64 m_waitingRequestId{0};
        bool m_cleanedUp{false};
    };

    // Stands in ScopeOne for what one DriverHost runs. Every call goes to the DriverHost as a
    // request on the transport thread.
    class DriverHostClient final : public QObject,
                                   public Device,
                                   public Camera,
                                   public Stage,
                                   public Shutter,
                                   public Autofocus,
                                   public StateDevice,
                                   public SignalSource,
                                   public Daq,
                                   public CameraFrameSource
    {
    public:
        DriverHostClient(QString providerId, QStringList arguments)
            : m_expectedProviderId(std::move(providerId))
              , m_arguments(std::move(arguments))
        {
        }

        ~DriverHostClient() override
        {
            if (m_worker && m_workerThread.isRunning())
            {
                DriverHostTransport* const worker = m_worker;
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
            m_worker = new DriverHostTransport(
                m_expectedProviderId,
                m_arguments,
                hostKey,
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
            DriverHostTransport* const worker = m_worker;
            QMetaObject::invokeMethod(
                worker,
                [this, worker, &initialized, &error]()
                {
                    QJsonObject description;
                    initialized = worker->initialize(m_descriptor,
                                                     m_devices,
                                                     m_signalSources,
                                                     m_daqDevices,
                                                     description,
                                                     &error);
                    m_defaultXYStage = description.value(driverhost::kDefaultXYStageField).toString();
                    m_defaultZStage = description.value(driverhost::kDefaultZStageField).toString();
                    m_defaultShutter = description.value(driverhost::kDefaultShutterField).toString();
                    m_defaultAutofocus = description.value(driverhost::kDefaultAutofocusField).toString();
                    for (const QJsonValue& value : description.value(driverhost::kDevicesField).toArray())
                    {
                        const QJsonObject device = value.toObject();
                        const QString deviceId = device.value(driverhost::kDeviceIdField).toString();
                        if (device.value(driverhost::kEventSequenceField).toBool())
                        {
                            m_sequenceCameras.insert(deviceId);
                        }
                        if (device.contains(driverhost::kPixelSizeField))
                        {
                            m_pixelSizes.insert(deviceId, device.value(driverhost::kPixelSizeField).toDouble());
                        }
                    }
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
                            SignalSource::StateSink state) override
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

        void setDaqSinks(InputSink input, Daq::StateSink state) override
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

        // The frames go to the CameraFrameDelivery instead
        void setFrameSink(FrameSink) override
        {
        }

        void setFrameDelivery(CameraFrameDelivery* delivery) override
        {
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, delivery]() { worker->setFrameDelivery(delivery); });
        }

        bool setFrameDeliveryMode(const QStringList& cameraIds, FrameDeliveryMode mode) override
        {
            QMutexLocker requestLocker(&m_requestMutex);
            bool ok = false;
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, cameraIds, mode, &ok]() { ok = worker->setFrameDeliveryMode(cameraIds, mode); });
            return ok;
        }

        void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused) override
        {
            QMutexLocker requestLocker(&m_requestMutex);
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, cameraIds, paused]() { worker->setFrameDeliveryPaused(cameraIds, paused); });
        }

        void setPreviewStateSink(PreviewStateSink sink) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_previewStateSink = std::move(sink);
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

        bool getExposure(const QString& cameraId, double& exposureMs) const override
        {
            QJsonObject response;
            if (!isCamera(cameraId) || !request(driverhost::kCommandGetExposure, cameraId, {}, response, nullptr))
            {
                return false;
            }
            exposureMs = response.value(QStringLiteral("exposureMs")).toDouble();
            return true;
        }

        bool setExposure(const QString& cameraId, double exposureMs) override
        {
            QJsonObject fields;
            fields.insert(QStringLiteral("value"), exposureMs);
            QJsonObject response;
            clearPropertyDetails();
            return isCamera(cameraId) && exposureMs > 0.0
                && request(driverhost::kCommandSetExposure, cameraId, fields, response, nullptr);
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
            // The values are kept here, so that reading from the cache never waits for the host.
            // A property that was never read or set is asked of the host once.
            const QString key = deviceId.trimmed() + QLatin1Char('\n') + name;
            // Only for ScopeOne drivers. The cache of MMCore in a camera host also follows the
            // property changes the device reports, which this one would hide.
            const bool keepsValues = m_arguments.contains(QStringLiteral("--driver"));
            if (fromCache && keepsValues)
            {
                QMutexLocker locker(&m_stateMutex);
                const auto cached = m_propertyValues.constFind(key);
                if (cached != m_propertyValues.cend())
                {
                    return cached.value();
                }
            }
            QJsonObject response;
            QJsonObject fields;
            fields.insert(QStringLiteral("name"), name);
            fields.insert(QStringLiteral("fromCache"), fromCache);
            if (!request(driverhost::kCommandGetProperty, deviceId, fields, response, nullptr))
            {
                return {};
            }
            const QString value = response.value(QStringLiteral("value")).toString();
            if (keepsValues)
            {
                QMutexLocker locker(&m_stateMutex);
                m_propertyValues.insert(key, value);
            }
            return value;
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
            clearPropertyDetails();
            const bool ok = request(driverhost::kCommandSetProperty, deviceId, fields, response, errorMessage);
            if (ok && m_arguments.contains(QStringLiteral("--driver")))
            {
                QMutexLocker locker(&m_stateMutex);
                m_propertyValues.insert(deviceId.trimmed() + QLatin1Char('\n') + name, value);
            }
            if (ok && isCamera(deviceId))
            {
                refreshPixelSizes({deviceId.trimmed()});
            }
            return ok;
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
            const bool ok = request(driverhost::kCommandSetRoi, cameraId, fields, response, nullptr);
            if (ok)
            {
                refreshPixelSizes({cameraId.trimmed()});
            }
            return ok;
        }

        bool clearROI(const QString& cameraId) override
        {
            QJsonObject response;
            const bool ok = request(driverhost::kCommandClearRoi, cameraId, {}, response, nullptr);
            if (ok)
            {
                refreshPixelSizes({cameraId.trimmed()});
            }
            return ok;
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
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, &ok, &frame, cameraId, timeoutMs]()
            {
                ok = worker->captureEventFrame(cameraId, frame, (std::max)(1, timeoutMs), nullptr);
            });
            return ok;
        }

        bool supportsEventSequence(const QString& cameraId) const override
        {
            return m_sequenceCameras.contains(cameraId.trimmed());
        }

        bool captureEventSequence(const QString& cameraId,
                                  int count,
                                  std::vector<ImageFrame>& frames,
                                  int timeoutMs) override
        {
            QMutexLocker requestLocker(&m_requestMutex);
            bool ok = false;
            DriverHostTransport* const worker = m_worker;
            const QString id = cameraId.trimmed();
            runOnWorker([worker, &ok, &frames, id, count, timeoutMs]()
            {
                ok = worker->captureEventSequence(id, count, frames, (std::max)(1, timeoutMs));
            });
            return ok;
        }

        // Read on the GUI thread whenever the scale bar is drawn, so it never waits for DriverHost
        double pixelSizeUm(const QString& cameraId) const override
        {
            QMutexLocker locker(&m_stateMutex);
            return m_pixelSizes.value(cameraId.trimmed());
        }

        long propertySequenceCapacity(const QString& deviceId, const QString& name) override
        {
            QJsonObject response;
            return request(driverhost::kCommandGetPropertySequenceCapacity, deviceId,
                           {{QStringLiteral("name"), name}}, response, nullptr)
                       ? static_cast<long>(response.value(QStringLiteral("capacity")).toDouble())
                       : 0;
        }

        bool startPropertySequence(const QString& deviceId,
                                   const QString& name,
                                   const QStringList& values,
                                   QString* errorMessage) override
        {
            QJsonObject response;
            return request(driverhost::kCommandStartPropertySequence, deviceId,
                           {{QStringLiteral("name"), name},
                            {QStringLiteral("values"), QJsonArray::fromStringList(values)}},
                           response, errorMessage);
        }

        void stopPropertySequence(const QString& deviceId, const QString& name) override
        {
            QJsonObject response;
            request(driverhost::kCommandStopPropertySequence, deviceId,
                    {{QStringLiteral("name"), name}}, response, nullptr);
        }

        QString defaultXYStage() const override { return m_defaultXYStage; }
        QString defaultZStage() const override { return m_defaultZStage; }

        long stageSequenceCapacity(const QString& deviceId) const override
        {
            QJsonObject response;
            return request(driverhost::kCommandGetStageSequenceCapacity, deviceId, {}, response, nullptr)
                       ? static_cast<long>(response.value(QStringLiteral("capacity")).toDouble())
                       : 0;
        }

        bool startStageSequence(const QString& deviceId,
                                const std::vector<double>& positions,
                                QString* errorMessage) override
        {
            QJsonArray values;
            for (const double position : positions)
            {
                values.append(position);
            }
            QJsonObject response;
            return request(driverhost::kCommandStartStageSequence, deviceId,
                           {{QStringLiteral("positions"), values}}, response, errorMessage);
        }

        void stopStageSequence(const QString& deviceId) override
        {
            QJsonObject response;
            request(driverhost::kCommandStopStageSequence, deviceId, {}, response, nullptr);
        }

        QString defaultShutter() const override { return m_defaultShutter; }

        bool autoShutter() const override
        {
            QJsonObject response;
            return request(driverhost::kCommandGetAutoShutter, {}, {}, response, nullptr)
                && response.value(QStringLiteral("enabled")).toBool();
        }

        bool setAutoShutter(bool enabled, QString* errorMessage) override
        {
            QJsonObject response;
            return request(driverhost::kCommandSetAutoShutter, {},
                           {{QStringLiteral("enabled"), enabled}}, response, errorMessage);
        }

        QString defaultAutofocus() const override { return m_defaultAutofocus; }

        // An autofocus can search for longer than a usual request may take
        bool runAutofocus(const QString& deviceId, QString* errorMessage) override
        {
            constexpr int kAutofocusTimeoutMs = 120000;
            QJsonObject response;
            return request(driverhost::kCommandRunAutofocus, deviceId, {}, response, errorMessage,
                           kAutofocusTimeoutMs);
        }

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
            const bool ok = request(driverhost::kCommandSetState, deviceId, fields, response, errorMessage);
            if (ok)
            {
                refreshPixelSizes(cameraIds());
            }
            return ok;
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

    private:
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

        bool request(const QString& type,
                     const QString& deviceId,
                     const QJsonObject& fields,
                     QJsonObject& response,
                     QString* errorMessage,
                     int timeoutMs = kControlRequestTimeoutMs) const
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
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, request, &response, &transported, &transportError, timeoutMs]()
            {
                transported = worker->sendRequest(request,
                                                  response,
                                                  timeoutMs,
                                                  &transportError);
            });
            if (!transported)
            {
                if (errorMessage) *errorMessage = transportError;
                return false;
            }
            return responseSucceeded(response, errorMessage);
        }

        // The type, limits and allowed values of a property, kept until a setting changes, as a
        // property browser asks for each of them separately and every request crosses processes
        QJsonObject propertyDetails(const QString& deviceId, const QString& name)
        {
            const QString key = deviceId.trimmed() + QChar(0x1f) + name;
            {
                QMutexLocker locker(&m_stateMutex);
                const auto cached = m_propertyDetails.constFind(key);
                if (cached != m_propertyDetails.cend())
                {
                    return cached.value();
                }
            }
            QJsonObject fields;
            fields.insert(QStringLiteral("name"), name);
            fields.insert(QStringLiteral("fromCache"), true);
            QJsonObject response;
            if (!request(driverhost::kCommandGetProperty, deviceId, fields, response, nullptr))
            {
                return {};
            }
            QMutexLocker locker(&m_stateMutex);
            m_propertyDetails.insert(key, response);
            return response;
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

        // A setting such as the binning or the objective changes the pixel size, which is read
        // again after it
        void refreshPixelSizes(const QStringList& cameraIds)
        {
            for (const QString& cameraId : cameraIds)
            {
                QJsonObject response;
                if (request(driverhost::kCommandGetPixelSize, cameraId, {}, response, nullptr))
                {
                    QMutexLocker locker(&m_stateMutex);
                    m_pixelSizes.insert(cameraId, response.value(driverhost::kPixelSizeField).toDouble());
                }
            }
        }

        void clearPropertyDetails()
        {
            QMutexLocker locker(&m_stateMutex);
            m_propertyDetails.clear();
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
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, &command, &config, &ok, &error]() { ok = worker->startStream(command, config, &error); });
            if (errorMessage) *errorMessage = error;
            return ok;
        }

        void stopStream(const QString& command, const QString& id)
        {
            QMutexLocker requestLocker(&m_requestMutex);
            DriverHostTransport* const worker = m_worker;
            runOnWorker([worker, &command, &id]() { worker->stopStream(command, id, nullptr); });
        }

        // The transport calls these on its thread; each hands the chunk or state to the sink Core set
        DriverHostTransport::StreamHandlers streamHandlers()
        {
            DriverHostTransport::StreamHandlers handlers;
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
                SignalSource::StateSink sink;
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
                Daq::StateSink sink;
                {
                    QMutexLocker locker(&m_sinkMutex);
                    sink = m_daqStateSink;
                }
                if (sink) sink(deviceId, state, message);
            };
            return handlers;
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

        QString m_expectedProviderId;
        QStringList m_arguments;
        HardwareProviderDescriptor m_descriptor;
        QList<HardwareDeviceDescriptor> m_devices;
        QString m_defaultXYStage;
        QString m_defaultZStage;
        QString m_defaultShutter;
        QString m_defaultAutofocus;
        QSet<QString> m_sequenceCameras;
        QThread m_workerThread;
        DriverHostTransport* m_worker{nullptr};
        mutable QMutex m_requestMutex;
        mutable QMutex m_sinkMutex;
        PreviewStateSink m_previewStateSink;
        TimeSeriesSink m_timeSeriesSink;
        TimestampedEventSink m_timestampedEventSink;
        SignalSource::StateSink m_signalStateSink;
        InputSink m_daqInputSink;
        Daq::StateSink m_daqStateSink;
        QList<SignalSourceDescriptor> m_signalSources;
        QList<DaqDeviceDescriptor> m_daqDevices;
        mutable QMutex m_stateMutex;
        QSet<QString> m_runningCameras;
        QHash<QString, QJsonObject> m_propertyDetails;
        QHash<QString, QString> m_propertyValues;
        QHash<QString, double> m_pixelSizes;
    };

    DevicePtr createScopeOneDevice(const QString& label,
                                   const QString& driverPath,
                                   const QString& deviceName,
                                   const QVariantMap& preInitProperties,
                                   QString* errorMessage)
    {
        const QString normalizedProviderId = label.trimmed();
        const QString normalizedDriverPath = QFileInfo(driverPath).absoluteFilePath();
        if (normalizedProviderId.isEmpty() || driverPath.trimmed().isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Label and driver path are required");
            return {};
        }
        if (!QFileInfo::exists(normalizedDriverPath))
        {
            if (errorMessage) *errorMessage = QStringLiteral("Driver not found: %1").arg(normalizedDriverPath);
            return {};
        }
        QStringList arguments{QStringLiteral("--driver"), normalizedDriverPath};
        if (!deviceName.isEmpty())
        {
            arguments << QStringLiteral("--device") << deviceName;
        }
        if (!preInitProperties.isEmpty())
        {
            arguments << QStringLiteral("--properties")
                      << QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(preInitProperties))
                                               .toJson(QJsonDocument::Compact));
        }
        return startDriverHost(normalizedProviderId, arguments, errorMessage);
    }

    DevicePtr startDriverHost(const QString& providerId,
                                        const QStringList& arguments,
                                        QString* errorMessage)
    {
        auto client = std::make_shared<DriverHostClient>(providerId, arguments);
        if (!client->initialize(errorMessage)) return {};
        return client;
    }
}
