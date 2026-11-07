#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QReadWriteLock>
#include <QSet>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QWinEventNotifier>
#include <windows.h>
#else
#include <QSocketNotifier>
#include <csignal>
#include <fcntl.h>
#ifdef Q_OS_MACOS
// The file system of macOS ignores case, so <semaphore.h> found the Semaphore.h of MMCore on the
// include path, and sem_unlink was undeclared. The system header declares it here.
#include <sys/semaphore.h>
#else
#include <semaphore.h>
#endif
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/event.h>
#else
#include <sys/syscall.h>
#endif
#endif

#ifndef Q_OS_WIN
// The macOS SDK declares shm_unlink only outside strict POSIX mode, and the build of DriverHost
// failed there on it. libc has it on every Unix, so it is declared here as the SDK does.
extern "C" int shm_unlink(const char* name);
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "internal/DriverHostProtocol.h"
#include "internal/MicroManagerCamera.h"
#include "internal/HardwareStreamCodec.h"
#include "internal/SharedFrameRing.h"
#include "internal/StreamRing.h"
#include "scopeone/Camera.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/Driver.h"
#include "scopeone/DriverManifest.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/SharedFrame.h"
#include "scopeone/SignalSource.h"

namespace scopeone::core::internal
{
    using scopeone::core::SharedFrameHeader;
    using scopeone::core::kSharedFrameMaxBytes;

    class ControlConnection final : public QObject
    {
        Q_OBJECT

    public:
        // Wrap one local control socket connection
        ControlConnection(quint64 connectionId, QLocalSocket* socket, QObject* parent = nullptr)
            : QObject(parent)
              , m_connectionId(connectionId)
              , m_socket(socket)
        {
            m_socket->setParent(this);

            connect(m_socket, &QLocalSocket::readyRead,
                    this, &ControlConnection::onReadyRead);
            connect(m_socket, &QLocalSocket::disconnected,
                    this, &ControlConnection::onDisconnected);
            connect(m_socket, &QLocalSocket::errorOccurred, this,
                    [this](QLocalSocket::LocalSocketError socketError)
                    {
                        // Core closing the connection at a normal unload is no error
                        if (socketError == QLocalSocket::PeerClosedError)
                        {
                            return;
                        }
                        qWarning().noquote()
                            << QString("DriverHost control socket error (%1) on connection %2")
                               .arg(static_cast<int>(socketError))
                               .arg(m_connectionId);
                    });
        }

        // Send one encoded protocol message to the client
        void sendMessage(const QJsonObject& message)
        {
            if (m_socket->state() != QLocalSocket::ConnectedState)
            {
                return;
            }
            m_socket->write(driverhost::encodeMessage(message));
        }

    signals:
        void requestReceived(quint64 connectionId,
                             quint64 requestId,
                             const QString& type,
                             const QJsonObject& message);
        void connectionClosed(quint64 connectionId);

    private slots:
        // Decode queued socket bytes into control requests
        void onReadyRead()
        {
            m_readBuffer += m_socket->readAll();
            while (true)
            {
                QJsonObject message;
                QString error;
                const driverhost::DecodeResult result =
                    driverhost::tryDecodeMessage(m_readBuffer, message, &error);
                if (result == driverhost::DecodeResult::Incomplete)
                {
                    return;
                }
                if (result == driverhost::DecodeResult::Error)
                {
                    qWarning().noquote()
                        << QString("DriverHost control protocol error on connection %1: %2")
                           .arg(m_connectionId)
                           .arg(error);
                    m_socket->disconnectFromServer();
                    return;
                }

                if (message.value(driverhost::kEnvelopeVersionField).toInt(0)
                    != static_cast<int>(driverhost::kProtocolVersion))
                {
                    qWarning().noquote()
                        << QString("DriverHost control protocol version mismatch on connection %1")
                        .arg(m_connectionId);
                    m_socket->disconnectFromServer();
                    return;
                }

                if (message.value(driverhost::kEnvelopeKindField).toString()
                    != driverhost::kMessageKindRequest)
                {
                    qWarning().noquote()
                        << QString("Ignoring non-request control message on connection %1")
                        .arg(m_connectionId);
                    continue;
                }

                const quint64 requestId =
                    driverhost::decodeUInt64(message.value(driverhost::kEnvelopeRequestIdField));
                const QString type = message.value(driverhost::kMessageTypeField).toString();
                if (requestId == 0 || type.isEmpty())
                {
                    qWarning().noquote()
                        << QString("Ignoring malformed request on connection %1")
                        .arg(m_connectionId);
                    continue;
                }

                emit requestReceived(m_connectionId, requestId, type, message);
            }
        }

        // Notify DriverHost when this connection closes
        void onDisconnected()
        {
            emit connectionClosed(m_connectionId);
            deleteLater();
        }

    private:
        quint64 m_connectionId{0};
        QLocalSocket* m_socket{nullptr};
        QByteArray m_readBuffer;
    };

    // Runs one device of a driver or one Micro-Manager camera, and answers the requests of
    // ScopeOne on a thread of its own
    class DriverRuntime final : public QObject
    {
        Q_OBJECT

    public:
        // A Micro-Manager camera is run when cameraSetup is set, the driver at driverPath otherwise
        DriverRuntime(QString driverPath,
                      std::optional<MicroManagerCameraSetup> cameraSetup,
                      QString providerId,
                      QString hostKey,
                      QString deviceName,
                      QVariantMap preInitProperties,
                      QObject* parent = nullptr)
            : QObject(parent)
              , m_driverPath(std::move(driverPath))
              , m_cameraSetup(std::move(cameraSetup))
              , m_providerId(std::move(providerId))
              , m_hostKey(std::move(hostKey))
              , m_deviceName(std::move(deviceName))
              , m_preInitProperties(std::move(preInitProperties))
        {
        }

        ~DriverRuntime() override
        {
            stopForExit();
        }

        bool initializeRuntime();
        QString lastError() const { return m_lastError; }
        void publishHello();
        void handleRequest(quint64 connectionId,
                           quint64 requestId,
                           const QString& type,
                           const QJsonObject& message);
        void stopForExit();

    signals:
        void responseReady(quint64 connectionId, const QJsonObject& response);
        void eventReady(const QJsonObject& event);
        void shutdownRequested();

    private:
        static QJsonObject makeResponse(const QString& type, quint64 requestId, bool ok)
        {
            QJsonObject response = driverhost::makeEnvelope(driverhost::kMessageKindResponse, type, requestId);
            response.insert(QStringLiteral("ok"), ok);
            return response;
        }

        static QJsonObject makeErrorResponse(const QString& type, quint64 requestId, const QString& error)
        {
            QJsonObject response = makeResponse(type, requestId, false);
            response.insert(QStringLiteral("error"), error);
            return response;
        }

        bool createDevice();

        struct CameraTransport
        {
            QString deviceId;
            QString shmKey;
            std::unique_ptr<QSharedMemory> shm;
            std::atomic<FrameDeliveryMode> deliveryMode{FrameDeliveryMode::PreviewLatest};
            std::atomic_bool paused{false};
            QMutex writeMutex;
            QElapsedTimer deliveryTimer;
            // The newest frame the live view has not been told about, and the timer that tells it
            quint64 pendingNotice{0};
            QTimer* noticeTimer{nullptr};
            std::atomic_bool noticeQueued{false};
            quint64 frameIndex{0};
            int nextWriteSlot{0};
            bool previewRunning{false};
        };

        QJsonObject makeEvent(const QString& type) const;
        QJsonArray capabilities(const HardwareDeviceDescriptor& device) const;
        bool supportsCapability(const HardwareDeviceDescriptor& device,
                                const QString& capability) const;
        QJsonObject deviceDescription(const HardwareDeviceDescriptor& device) const;
        const HardwareDeviceDescriptor* requestedDevice(const QJsonObject& message) const;
        std::shared_ptr<CameraTransport> cameraTransport(const QString& deviceId) const;
        bool createSharedMemory(CameraTransport& transport);
        void writeCameraFrame(const ImageFrame& frame);
        void releaseSharedMemoryName(const QString& key);
        void sendPendingNotice(const QString& deviceId);
        bool writeFrame(CameraTransport& transport,
                        const ImageFrame& frame,
                        quint64* frameIndexOut = nullptr);
        void emitPreviewStateEvent(const QString& deviceId, bool running);
        void emitFrameAvailableEvent(const QString& deviceId, quint64 frameIndex);

        // The ring a signal source or a DAQ device writes its chunks to while it runs
        struct StreamTransport
        {
            QString streamId;
            QString shmKey;
            QSharedMemory shm;
            streamring::Writer writer;
            QMutex writeMutex;
            std::atomic_bool notifyQueued{false};
            bool stopRequested{false};
        };

        void installStreamSinks();
        bool handleStreamRequest(quint64 requestId,
                                 const QString& type,
                                 const QJsonObject& message,
                                 const std::function<void(const QJsonObject&)>& reply);
        std::shared_ptr<StreamTransport> openStream(const QString& streamId,
                                                    qsizetype capacity,
                                                    QString* errorMessage);
        void closeStream(const QString& streamId);
        void markStreamStopped(const QString& streamId);
        void releaseEndedStream(const QString& streamId, bool failed);
        void writeStream(const QString& streamId, streamring::RecordType type, const QByteArray& payload);
        qsizetype signalStreamCapacity(const SignalAcquisitionConfig& config) const;
        static qsizetype daqStreamCapacity(const DaqSessionConfig& config);

        QString m_driverPath;
        std::optional<MicroManagerCameraSetup> m_cameraSetup;
        QString m_providerId;
        QString m_hostKey;
        QString m_deviceName;
        QVariantMap m_preInitProperties;
        QString m_lastError;
        HardwareProviderDescriptor m_descriptor;
        QList<HardwareDeviceDescriptor> m_devices;
        QHash<QString, HardwareDeviceDescriptor> m_devicesById;
        HardwareDeviceDescriptor m_rootDevice;
        QHash<QString, std::shared_ptr<CameraTransport>> m_cameraTransports;
        mutable QReadWriteLock m_cameraTransportsLock;
        std::unique_ptr<QPluginLoader> m_driverLoader;
        DevicePtr m_device;
        std::shared_ptr<MicroManagerCamera> m_microManagerCamera;
        Camera* m_camera{nullptr};
        Device* m_properties{nullptr};
        Stage* m_stage{nullptr};
        Shutter* m_shutter{nullptr};
        Autofocus* m_autofocus{nullptr};
        StateDevice* m_state{nullptr};
        SignalSource* m_signals{nullptr};
        Daq* m_daq{nullptr};
        QList<SignalSourceDescriptor> m_signalSources;
        QList<DaqDeviceDescriptor> m_daqDevices;
        QHash<QString, std::shared_ptr<StreamTransport>> m_streams;
        QMutex m_streamsMutex;
        quint64 m_nextStreamIndex{0};
        // The images of the last sequence of each camera, until SendEventFrames has written them
        QHash<QString, std::vector<ImageFrame>> m_eventSequences;
        std::atomic_bool m_acceptFrames{false};
        std::atomic_bool m_acceptStreams{false};
    };

    // A Micro-Manager camera is built into DriverHost, a device of a driver comes from its library
    bool DriverRuntime::createDevice()
    {
        if (m_cameraSetup)
        {
            m_microManagerCamera = MicroManagerCamera::load(*m_cameraSetup, true, &m_lastError);
            m_device = m_microManagerCamera;
            return m_device != nullptr;
        }
        m_driverLoader = std::make_unique<QPluginLoader>(m_driverPath);
        DriverManifest manifest;
        if (!parseDriverManifest(
                m_driverLoader->metaData().value(QStringLiteral("MetaData")).toObject(),
                manifest,
                &m_lastError))
        {
            return false;
        }
        QObject* const instance = m_driverLoader->instance();
        if (!instance)
        {
            m_lastError = m_driverLoader->errorString();
            return false;
        }

        auto* const factory = qobject_cast<Driver*>(instance);
        if (!factory)
        {
            m_lastError = QStringLiteral("Module does not implement Driver");
            return false;
        }
        if (factory->driverId() != manifest.id)
        {
            m_lastError = QStringLiteral("Driver identity mismatch");
            return false;
        }

        const QStringList deviceNames = factory->deviceNames();
        const QString deviceName = m_deviceName.isEmpty() && !deviceNames.isEmpty()
                                       ? deviceNames.first()
                                       : m_deviceName;
        // A driver that lists no names checks the name in createDevice itself
        if (!deviceNames.isEmpty() && !deviceNames.contains(deviceName))
        {
            m_lastError = QStringLiteral("The driver has no device %1").arg(deviceName);
            return false;
        }
        m_device = factory->createDevice(deviceName, m_preInitProperties, &m_lastError);
        if (!m_device)
        {
            if (m_lastError.isEmpty())
            {
                m_lastError = QStringLiteral("The driver returned no device");
            }
            return false;
        }
        return true;
    }

    bool DriverRuntime::initializeRuntime()
    {
        m_lastError.clear();
        if (!createDevice())
        {
            return false;
        }
        // ScopeOne names the provider, which is the cfg label of a driver device
        m_descriptor = m_device->descriptor();
        m_descriptor.id = m_providerId;

        Device* const provider = m_device.get();
        m_camera = dynamic_cast<Camera*>(provider);
        m_properties = provider;
        m_stage = dynamic_cast<Stage*>(provider);
        m_shutter = dynamic_cast<Shutter*>(provider);
        m_autofocus = dynamic_cast<Autofocus*>(provider);
        m_state = dynamic_cast<StateDevice*>(provider);
        m_signals = dynamic_cast<SignalSource*>(provider);
        m_daq = dynamic_cast<Daq*>(provider);
        // A device without peripherals is one device under its label, and its kind follows its capability
        m_rootDevice.name = m_providerId;
        m_rootDevice.kind = m_shutter ? HardwareDeviceKind::Shutter : HardwareDeviceKind::Generic;
        m_rootDevice.state = HardwareDeviceState::Initialized;

        QSet<QString> logicalIds;
        int cameraIndex = 0;
        for (HardwareDeviceDescriptor device : m_device->devices())
        {
            const QString logicalId = device.logicalId.trimmed();
            const bool capabilityAvailable =
                (device.kind != HardwareDeviceKind::Camera || m_camera)
                && ((device.kind != HardwareDeviceKind::XYStage
                     && device.kind != HardwareDeviceKind::ZStage)
                    || m_stage)
                && (device.kind != HardwareDeviceKind::Shutter || m_shutter)
                && (device.kind != HardwareDeviceKind::State || m_state)
                && (device.kind != HardwareDeviceKind::AutoFocus || m_autofocus);
            if (logicalId.isEmpty()
                || device.logicalId != logicalId
                || logicalIds.contains(logicalId)
                || !capabilityAvailable)
            {
                m_lastError = QStringLiteral("Provider returned an invalid device catalog");
                return false;
            }
            logicalIds.insert(logicalId);
            device.providerId = m_providerId;
            device.endpoint = HardwareEndpointKind::DriverHost;
            m_devices.append(device);
            m_devicesById.insert(logicalId, device);

            if (device.kind == HardwareDeviceKind::Camera)
            {
                auto transport = std::make_shared<CameraTransport>();
                transport->deviceId = logicalId;
                transport->shmKey = driverhost::sharedMemoryKey(m_hostKey, cameraIndex++);
                transport->noticeTimer = new QTimer(this);
                transport->noticeTimer->setSingleShot(true);
                transport->noticeTimer->setTimerType(Qt::PreciseTimer);
                connect(transport->noticeTimer, &QTimer::timeout, this,
                        [this, logicalId]() { sendPendingNotice(logicalId); });
                if (!createSharedMemory(*transport))
                {
                    return false;
                }
                m_cameraTransports.insert(logicalId, std::move(transport));
            }
        }

        if (m_camera && !m_cameraTransports.isEmpty())
        {
            m_acceptFrames.store(true, std::memory_order_release);
            m_camera->setFrameSink([this](const ImageFrame& frame) { writeCameraFrame(frame); });
        }
        if (m_microManagerCamera)
        {
            const QString cameraId = m_microManagerCamera->cameraId();
            m_microManagerCamera->setErrorSink([this, cameraId](const QString& error)
            {
                QMetaObject::invokeMethod(this, [this, cameraId, error]()
                {
                    if (const auto transport = cameraTransport(cameraId))
                    {
                        transport->previewRunning = false;
                    }
                    emitPreviewStateEvent(cameraId, false);
                    QJsonObject event = makeEvent(driverhost::kEventDriverHostError);
                    event.insert(QStringLiteral("error"), error);
                    emit eventReady(event);
                }, Qt::QueuedConnection);
            });
        }
        installStreamSinks();
        return true;
    }

    void DriverRuntime::installStreamSinks()
    {
        if (m_signals)
        {
            m_signalSources = m_signals->signalSources();
            m_signals->setSignalSinks(
                [this](const TimeSeriesChunk& chunk)
                {
                    writeStream(QStringLiteral("signal:") + chunk.sourceId,
                                streamring::RecordType::TimeSeries, hardwarestream::encode(chunk));
                },
                [this](const TimestampedEventChunk& chunk)
                {
                    writeStream(QStringLiteral("signal:") + chunk.sourceId,
                                streamring::RecordType::TimestampedEvents, hardwarestream::encode(chunk));
                },
                [this](const QString& sourceId, SignalSourceState state, const QString& message)
                {
                    QMetaObject::invokeMethod(this, [this, sourceId, state, message]()
                    {
                        QJsonObject event = makeEvent(driverhost::kEventSignalState);
                        event.insert(driverhost::kSourceIdField, sourceId);
                        event.insert(driverhost::kStateField, static_cast<int>(state));
                        event.insert(driverhost::kStateMessageField, message);
                        emit eventReady(event);
                        if (state == SignalSourceState::Idle || state == SignalSourceState::Error)
                        {
                            releaseEndedStream(QStringLiteral("signal:") + sourceId,
                                               state == SignalSourceState::Error);
                        }
                    }, Qt::QueuedConnection);
                });
        }
        if (m_daq)
        {
            m_daqDevices = m_daq->daqDevices();
            m_daq->setDaqSinks(
                [this](const DaqInputChunk& chunk)
                {
                    writeStream(QStringLiteral("daq:") + chunk.deviceId,
                                streamring::RecordType::DaqInput, hardwarestream::encode(chunk));
                },
                [this](const QString& deviceId, DaqState state, const QString& message)
                {
                    QMetaObject::invokeMethod(this, [this, deviceId, state, message]()
                    {
                        QJsonObject event = makeEvent(driverhost::kEventDaqState);
                        event.insert(driverhost::kDeviceIdField, deviceId);
                        event.insert(driverhost::kStateField, static_cast<int>(state));
                        event.insert(driverhost::kStateMessageField, message);
                        emit eventReady(event);
                        if (state == DaqState::Idle || state == DaqState::Error)
                        {
                            releaseEndedStream(QStringLiteral("daq:") + deviceId, state == DaqState::Error);
                        }
                    }, Qt::QueuedConnection);
                });
        }
        m_acceptStreams.store(m_signals || m_daq, std::memory_order_release);
    }

    std::shared_ptr<DriverRuntime::StreamTransport> DriverRuntime::openStream(
        const QString& streamId,
        qsizetype capacity,
        QString* errorMessage)
    {
        auto stream = std::make_shared<StreamTransport>();
        stream->streamId = streamId;
        stream->shmKey = driverhost::streamSharedMemoryKey(m_hostKey, m_nextStreamIndex++);
        if (!streamring::createSharedRing(stream->shm, driverhost::sharedMemoryNativeKey(stream->shmKey),
                                          capacity, errorMessage))
        {
            return {};
        }
        stream->writer = streamring::Writer(stream->shm.data());
        QMutexLocker locker(&m_streamsMutex);
        m_streams.insert(streamId, stream);
        return stream;
    }

    void DriverRuntime::closeStream(const QString& streamId)
    {
        QMutexLocker locker(&m_streamsMutex);
        m_streams.remove(streamId);
    }

    void DriverRuntime::markStreamStopped(const QString& streamId)
    {
        QMutexLocker locker(&m_streamsMutex);
        if (const auto stream = m_streams.value(streamId))
        {
            stream->stopRequested = true;
        }
    }

    // A stopped stream kept its ring until the next start of the same source, up to 256 MB in
    // each process per source. The ring goes once the source has ended, and only the ring that
    // was stopped, as the end of an earlier run can arrive after the next start.
    void DriverRuntime::releaseEndedStream(const QString& streamId, bool failed)
    {
        QMutexLocker locker(&m_streamsMutex);
        const auto stream = m_streams.value(streamId);
        if (stream && (stream->stopRequested || failed))
        {
            m_streams.remove(streamId);
        }
    }

    void DriverRuntime::writeStream(const QString& streamId,
                                            streamring::RecordType type,
                                            const QByteArray& payload)
    {
        if (!m_acceptStreams.load(std::memory_order_acquire))
        {
            return;
        }
        std::shared_ptr<StreamTransport> stream;
        {
            QMutexLocker locker(&m_streamsMutex);
            stream = m_streams.value(streamId);
        }
        if (!stream)
        {
            return;
        }
        {
            QMutexLocker locker(&stream->writeMutex);
            stream->writer.write(type, payload.constData(), static_cast<std::size_t>(payload.size()));
        }
        // One notice stands for every record written until it is sent, as the reader drains the ring
        if (!stream->notifyQueued.exchange(true))
        {
            QMetaObject::invokeMethod(this, [this, stream]()
            {
                stream->notifyQueued.store(false);
                QJsonObject event = makeEvent(driverhost::kEventStreamAvailable);
                event.insert(driverhost::kStreamIdField, stream->streamId);
                emit eventReady(event);
            }, Qt::QueuedConnection);
        }
    }

    // Two seconds of the stream. Photon events have no fixed rate, so their rings are sized for
    // bright scans instead.
    qsizetype DriverRuntime::signalStreamCapacity(const SignalAcquisitionConfig& config) const
    {
        constexpr qsizetype kMinimum = 4 * 1024 * 1024;
        constexpr qsizetype kEvents = 64 * 1024 * 1024;
        constexpr qsizetype kMaximum = 256 * 1024 * 1024;
        const auto source = std::find_if(m_signalSources.cbegin(), m_signalSources.cend(),
                                         [&config](const SignalSourceDescriptor& descriptor)
                                         {
                                             return descriptor.id == config.sourceId;
                                         });
        if (config.scanImage.enabled || config.publishTimestampedEvents
            || (source != m_signalSources.cend() && source->streamType == SignalStreamType::TimestampedEvents))
        {
            return kEvents;
        }
        const double bytesPerSecond = sizeof(double) / (std::max)(config.sampleIntervalSeconds, 1.0e-9);
        return std::clamp(static_cast<qsizetype>(2.0 * std::min(bytesPerSecond, 1.0e12)), kMinimum, kMaximum);
    }

    qsizetype DriverRuntime::daqStreamCapacity(const DaqSessionConfig& config)
    {
        constexpr qsizetype kMinimum = 4 * 1024 * 1024;
        constexpr qsizetype kMaximum = 256 * 1024 * 1024;
        double bytesPerSecond = 0.0;
        for (const DaqAnalogTaskConfig& task : config.analogTasks)
        {
            if (task.direction == DaqTaskDirection::Input)
            {
                bytesPerSecond += task.timing.sampleRateHz * task.channels.size() * sizeof(double);
            }
        }
        for (const DaqDigitalTaskConfig& task : config.digitalTasks)
        {
            if (task.direction == DaqTaskDirection::Input)
            {
                bytesPerSecond += task.timing.sampleRateHz * sizeof(quint32);
            }
        }
        return std::clamp(static_cast<qsizetype>(2.0 * std::min(bytesPerSecond, 1.0e12)), kMinimum, kMaximum);
    }

    bool DriverRuntime::handleStreamRequest(quint64 requestId,
                                                    const QString& type,
                                                    const QJsonObject& message,
                                                    const std::function<void(const QJsonObject&)>& reply)
    {
        const bool signalCommand = type == driverhost::kCommandStartSignal
            || type == driverhost::kCommandStopSignal;
        const bool daqCommand = type == driverhost::kCommandStartDaq || type == driverhost::kCommandStopDaq;
        if (!signalCommand && !daqCommand)
        {
            return false;
        }
        if ((signalCommand && !m_signals) || (daqCommand && !m_daq))
        {
            reply(makeErrorResponse(type, requestId, QStringLiteral("Provider does not support %1")
                                                         .arg(signalCommand ? QStringLiteral("signal sources")
                                                                            : QStringLiteral("DAQ devices"))));
            return true;
        }
        if (type == driverhost::kCommandStopSignal)
        {
            const QString sourceId = message.value(driverhost::kSourceIdField).toString();
            markStreamStopped(QStringLiteral("signal:") + sourceId);
            m_signals->stopSignal(sourceId);
            reply(makeResponse(type, requestId, true));
            return true;
        }
        if (type == driverhost::kCommandStopDaq)
        {
            const QString deviceId = message.value(driverhost::kDeviceIdField).toString();
            markStreamStopped(QStringLiteral("daq:") + deviceId);
            m_daq->stopDaq(deviceId);
            reply(makeResponse(type, requestId, true));
            return true;
        }

        const QJsonObject configObject = message.value(driverhost::kConfigField).toObject();
        QString streamId;
        qsizetype capacity = 0;
        SignalAcquisitionConfig signalConfig;
        DaqSessionConfig daqConfig;
        if (signalCommand)
        {
            signalConfig = hardwarestream::signalAcquisitionConfigFromJson(configObject);
            streamId = QStringLiteral("signal:") + signalConfig.sourceId;
            capacity = signalStreamCapacity(signalConfig);
        }
        else
        {
            daqConfig = hardwarestream::daqSessionConfigFromJson(configObject);
            streamId = QStringLiteral("daq:") + daqConfig.deviceId;
            capacity = daqStreamCapacity(daqConfig);
        }
        QString error;
        const auto stream = openStream(streamId, capacity, &error);
        const bool started = stream
            && (signalCommand ? m_signals->startSignal(signalConfig, &error)
                              : m_daq->startDaq(daqConfig, &error));
        if (!started)
        {
            closeStream(streamId);
            reply(makeErrorResponse(type, requestId, error.isEmpty() ? QStringLiteral("Start failed") : error));
            return true;
        }
        QJsonObject response = makeResponse(type, requestId, true);
        response.insert(driverhost::kStreamIdField, streamId);
        response.insert(driverhost::kSharedMemoryKeyField, stream->shmKey);
        reply(response);
        return true;
    }

    QJsonObject DriverRuntime::makeEvent(const QString& type) const
    {
        QJsonObject event = driverhost::makeEnvelope(driverhost::kMessageKindEvent, type);
        event.insert(driverhost::kProviderIdField, m_providerId);
        return event;
    }

    QJsonArray DriverRuntime::capabilities(
        const HardwareDeviceDescriptor& device) const
    {
        QJsonArray result;
        if (m_camera && device.kind == HardwareDeviceKind::Camera)
        {
            result.append(driverhost::kCapabilityCamera);
        }
        if (m_properties) result.append(driverhost::kCapabilityProperties);
        if (m_stage
            && (device.kind == HardwareDeviceKind::XYStage
                || device.kind == HardwareDeviceKind::ZStage))
        {
            result.append(driverhost::kCapabilityStage);
        }
        if (m_shutter && device.kind == HardwareDeviceKind::Shutter)
        {
            result.append(driverhost::kCapabilityShutter);
        }
        if (m_state && device.kind == HardwareDeviceKind::State)
        {
            result.append(driverhost::kCapabilityState);
        }
        if (m_autofocus && device.kind == HardwareDeviceKind::AutoFocus)
        {
            result.append(driverhost::kCapabilityAutofocus);
        }
        return result;
    }

    bool DriverRuntime::supportsCapability(
        const HardwareDeviceDescriptor& device,
        const QString& capability) const
    {
        return capabilities(device).contains(QJsonValue(capability));
    }

    QJsonObject DriverRuntime::deviceDescription(
        const HardwareDeviceDescriptor& device) const
    {
        QJsonObject object;
        object.insert(driverhost::kDeviceIdField, device.logicalId);
        object.insert(driverhost::kProviderDeviceIdField, device.providerDeviceId);
        object.insert(driverhost::kHardwareIdField, device.hardwareId);
        object.insert(driverhost::kDeviceNameField, device.name);
        object.insert(driverhost::kDeviceKindField, driverhost::deviceKindName(device.kind));
        object.insert(driverhost::kDeviceStateField, static_cast<int>(device.state));
        object.insert(driverhost::kDevicePropertiesField,
                      QJsonObject::fromVariantMap(device.properties));
        object.insert(driverhost::kCapabilitiesField, capabilities(device));
        if (const auto transport = cameraTransport(device.logicalId))
        {
            object.insert(driverhost::kSharedMemoryKeyField, transport->shmKey);
            object.insert(driverhost::kEventSequenceField, m_camera->supportsEventSequence(device.logicalId));
            object.insert(driverhost::kPixelSizeField, m_camera->pixelSizeUm(device.logicalId));
        }
        return object;
    }

    const HardwareDeviceDescriptor* DriverRuntime::requestedDevice(
        const QJsonObject& message) const
    {
        const QString deviceId = message.value(driverhost::kDeviceIdField)
                                     .toString().trimmed();
        // A request without a device id addresses the Device itself, whose properties a driver
        // with no peripherals has. It was answered as an unknown device before.
        if (deviceId.isEmpty())
        {
            return &m_rootDevice;
        }
        const auto it = m_devicesById.constFind(deviceId);
        return it == m_devicesById.cend() ? nullptr : &it.value();
    }

    std::shared_ptr<DriverRuntime::CameraTransport>
    DriverRuntime::cameraTransport(const QString& deviceId) const
    {
        QReadLocker locker(&m_cameraTransportsLock);
        return m_cameraTransports.value(deviceId.trimmed());
    }

    void DriverRuntime::publishHello()
    {
        emit eventReady(makeEvent(driverhost::kEventHello));
    }

    bool DriverRuntime::createSharedMemory(CameraTransport& transport)
    {
        transport.shm = std::make_unique<QSharedMemory>(driverhost::sharedMemoryNativeKey(transport.shmKey));
        return sharedframe::createRing(*transport.shm, &m_lastError);
    }

    // A frame may point into a buffer of the driver, so it is written to shared memory before the
    // call returns. The live view is told about new frames at most every 16 ms, and the notice it
    // would miss comes from a timer, so the last frame of a burst is shown too.
    void DriverRuntime::writeCameraFrame(const ImageFrame& frame)
    {
        if (!m_acceptFrames.load(std::memory_order_acquire))
        {
            return;
        }
        const QString deviceId = frame.cameraId.trimmed();
        const auto transport = cameraTransport(deviceId);
        if (!transport || transport->paused.load(std::memory_order_relaxed))
        {
            return;
        }
        quint64 frameIndex = 0;
        int noticeDelayMs = 0;
        {
            QMutexLocker locker(&transport->writeMutex);
            if (!writeFrame(*transport, frame, &frameIndex))
            {
                return;
            }
            if (transport->deliveryMode.load(std::memory_order_relaxed) == FrameDeliveryMode::PreviewLatest
                && transport->deliveryTimer.isValid())
            {
                noticeDelayMs = kPreviewFrameIntervalMs - static_cast<int>(transport->deliveryTimer.elapsed());
            }
            if (noticeDelayMs > 0)
            {
                transport->pendingNotice = frameIndex;
            }
            else
            {
                transport->pendingNotice = 0;
                transport->deliveryTimer.restart();
            }
        }
        if (noticeDelayMs <= 0)
        {
            emitFrameAvailableEvent(deviceId, frameIndex);
        }
        else if (!transport->noticeQueued.exchange(true))
        {
            QTimer* const timer = transport->noticeTimer;
            QMetaObject::invokeMethod(timer, [timer, noticeDelayMs]() { timer->start(noticeDelayMs); },
                                      Qt::QueuedConnection);
        }
    }

    // Removes the name of a ring of this host. Qt never removes the name of POSIX shared memory,
    // which then outlived a crash of both processes; removed once ScopeOne has attached, it goes
    // away with the last mapping.
    void DriverRuntime::releaseSharedMemoryName(const QString& key)
    {
#ifndef Q_OS_WIN
        bool owned = false;
        {
            QReadLocker locker(&m_cameraTransportsLock);
            for (const auto& transport : std::as_const(m_cameraTransports))
            {
                owned = owned || transport->shmKey == key;
            }
        }
        {
            QMutexLocker locker(&m_streamsMutex);
            for (const auto& stream : std::as_const(m_streams))
            {
                owned = owned || stream->shmKey == key;
            }
        }
        if (owned)
        {
            // Qt guards the memory with a POSIX semaphore of the same name, whose name stays too
            const QByteArray name = driverhost::sharedMemoryNativeKey(key).nativeKey().toLocal8Bit();
            ::shm_unlink(name.constData());
            ::sem_unlink(name.constData());
        }
#else
        (void)key;
#endif
    }

    void DriverRuntime::sendPendingNotice(const QString& deviceId)
    {
        const auto transport = cameraTransport(deviceId);
        if (!transport)
        {
            return;
        }
        transport->noticeQueued = false;
        quint64 frameIndex = 0;
        {
            QMutexLocker locker(&transport->writeMutex);
            frameIndex = std::exchange(transport->pendingNotice, 0);
            if (frameIndex != 0)
            {
                transport->deliveryTimer.restart();
            }
        }
        if (frameIndex != 0)
        {
            emitFrameAvailableEvent(deviceId, frameIndex);
        }
    }

    // The caller holds the write mutex of the transport
    bool DriverRuntime::writeFrame(CameraTransport& transport,
                                           const ImageFrame& frame,
                                           quint64* frameIndexOut)
    {
        if (!transport.shm || !transport.shm->isAttached() || !frame.isValid()
            || frame.cameraId.trimmed() != transport.deviceId
            || frame.bytes.size() > kSharedFrameMaxBytes)
        {
            return false;
        }
        transport.frameIndex = (std::max)(transport.frameIndex + 1, frame.frameIndex);
        SharedFrameHeader header = toSharedFrameHeader(frame);
        header.frameIndex = transport.frameIndex;
        if (header.timestampNs == 0)
        {
            header.timestampNs =
                static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }
        if (!sharedframe::writeFrame(static_cast<uchar*>(transport.shm->data()),
                                     transport.nextWriteSlot,
                                     header,
                                     frame.bytes.constData(),
                                     static_cast<std::size_t>(frame.bytes.size())))
        {
            return false;
        }
        if (frameIndexOut)
        {
            *frameIndexOut = transport.frameIndex;
        }
        return true;
    }

    void DriverRuntime::emitFrameAvailableEvent(const QString& deviceId, quint64 frameIndex)
    {
        QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
        event.insert(driverhost::kDeviceIdField, deviceId);
        event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
        emit eventReady(event);
    }

    void DriverRuntime::emitPreviewStateEvent(const QString& deviceId, bool running)
    {
        QJsonObject event = makeEvent(driverhost::kEventPreviewState);
        event.insert(driverhost::kDeviceIdField, deviceId);
        event.insert(QStringLiteral("running"), running);
        emit eventReady(event);
    }

    void DriverRuntime::handleRequest(quint64 connectionId,
                                              quint64 requestId,
                                              const QString& type,
                                              const QJsonObject& message)
    {
        const auto reply = [this, connectionId](const QJsonObject& response)
        {
            emit responseReady(connectionId, response);
        };
        const auto unsupported = [this, requestId, &type, &reply](const QString& capability)
        {
            reply(makeErrorResponse(type,
                                    requestId,
                                    QStringLiteral("Provider does not support %1").arg(capability)));
        };

        if (type == driverhost::kCommandDescribe)
        {
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(driverhost::kProviderIdField, m_providerId);
            response.insert(driverhost::kProviderNameField, m_descriptor.name);
            response.insert(driverhost::kProviderVersionField, m_descriptor.version);
            QJsonArray devices;
            for (const HardwareDeviceDescriptor& device : m_devices)
            {
                devices.append(deviceDescription(device));
            }
            if (m_devices.isEmpty() && !m_microManagerCamera)
            {
                devices.append(deviceDescription(m_rootDevice));
            }
            response.insert(driverhost::kDevicesField, devices);
            QJsonArray signalSources;
            for (const SignalSourceDescriptor& source : m_signalSources)
            {
                signalSources.append(hardwarestream::toJson(source));
            }
            response.insert(driverhost::kSignalSourcesField, signalSources);
            QJsonArray daqDevices;
            for (const DaqDeviceDescriptor& daqDevice : m_daqDevices)
            {
                daqDevices.append(hardwarestream::toJson(daqDevice));
            }
            response.insert(driverhost::kDaqDevicesField, daqDevices);
            if (m_stage)
            {
                response.insert(driverhost::kDefaultXYStageField, m_stage->defaultXYStage());
                response.insert(driverhost::kDefaultZStageField, m_stage->defaultZStage());
            }
            if (m_shutter)
            {
                response.insert(driverhost::kDefaultShutterField, m_shutter->defaultShutter());
            }
            if (m_autofocus)
            {
                response.insert(driverhost::kDefaultAutofocusField, m_autofocus->defaultAutofocus());
            }
            reply(response);
            return;
        }
        if (type == driverhost::kCommandReleaseSharedMemory)
        {
            for (const QJsonValue& value : message.value(driverhost::kKeysField).toArray())
            {
                releaseSharedMemoryName(value.toString());
            }
            reply(makeResponse(type, requestId, true));
            return;
        }
        if (type == driverhost::kCommandShutdown)
        {
            stopForExit();
            reply(makeResponse(type, requestId, true));
            QTimer::singleShot(50, this, [this]() { emit shutdownRequested(); });
            return;
        }
        if (!m_device)
        {
            reply(makeErrorResponse(type, requestId, QStringLiteral("Provider is not initialized")));
            return;
        }
        if (handleStreamRequest(requestId, type, message, reply))
        {
            return;
        }

        if (type == driverhost::kCommandGetAutoShutter || type == driverhost::kCommandSetAutoShutter)
        {
            if (!m_shutter)
            {
                unsupported(driverhost::kCapabilityShutter);
                return;
            }
            if (type == driverhost::kCommandGetAutoShutter)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("enabled"), m_shutter->autoShutter());
                reply(response);
                return;
            }
            QString error;
            const bool ok = m_shutter->setAutoShutter(message.value(QStringLiteral("enabled")).toBool(), &error);
            reply(ok ? makeResponse(type, requestId, true) : makeErrorResponse(type, requestId, error));
            return;
        }

        const HardwareDeviceDescriptor* const device = requestedDevice(message);
        if (!device)
        {
            reply(makeErrorResponse(type, requestId, QStringLiteral("Unknown provider device")));
            return;
        }
        const QString deviceId = device ? device->logicalId : QString{};

        if (type == driverhost::kCommandSetFrameDeliveryMode)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            const auto transport = cameraTransport(deviceId);
            if (!transport)
            {
                reply(makeErrorResponse(type, requestId,
                                        QStringLiteral("Camera transport is unavailable")));
                return;
            }
            const QString mode = message.value(QStringLiteral("mode")).toString();
            FrameDeliveryMode deliveryMode = FrameDeliveryMode::PreviewLatest;
            if (mode == driverhost::kFrameDeliveryModeLatestOnly)
            {
                deliveryMode = FrameDeliveryMode::LatestOnly;
            }
            else if (mode == driverhost::kFrameDeliveryModeAllFrames)
            {
                deliveryMode = FrameDeliveryMode::AllFrames;
            }
            else if (mode != driverhost::kFrameDeliveryModePreviewLatest)
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Unknown frame delivery mode")));
                return;
            }
            transport->deliveryMode.store(deliveryMode, std::memory_order_relaxed);
            if (m_microManagerCamera)
            {
                m_microManagerCamera->setFrameDeliveryMode(deliveryMode);
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("mode"), mode);
            reply(response);
            return;
        }

        if (type == driverhost::kCommandSetFrameDeliveryPaused)
        {
            const auto transport = cameraTransport(deviceId);
            if (!transport)
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            const bool paused = message.value(QStringLiteral("paused")).toBool();
            transport->paused.store(paused, std::memory_order_relaxed);
            if (m_microManagerCamera)
            {
                m_microManagerCamera->setFrameDeliveryPaused(paused);
            }
            reply(makeResponse(type, requestId, true));
            return;
        }

        if (type == driverhost::kCommandStartPreview
            || type == driverhost::kCommandStopPreview)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            const bool start = type == driverhost::kCommandStartPreview;
            // Images of a sequence the client gave up on would otherwise stay in memory
            m_eventSequences.remove(deviceId);
            const bool ok = start
                                ? m_camera->startPreviewFor(deviceId)
                                : m_camera->stopPreviewFor(deviceId);
            if (!ok)
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider rejected preview request")));
                return;
            }
            if (const auto transport = cameraTransport(deviceId))
            {
                transport->previewRunning = start;
            }
            reply(makeResponse(type, requestId, true));
            emitPreviewStateEvent(deviceId, start);
            return;
        }

        if (type == driverhost::kCommandGetExposure)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            double exposureMs = 0.0;
            if (!m_camera->getExposure(deviceId, exposureMs))
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider failed to read exposure")));
                return;
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("exposureMs"), exposureMs);
            reply(response);
            return;
        }

        if (type == driverhost::kCommandSetExposure)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            const double exposureMs = message.value(QStringLiteral("value")).toDouble(-1.0);
            double actualExposureMs = 0.0;
            if (exposureMs <= 0.0
                || !m_camera->setExposure(deviceId, exposureMs)
                || !m_camera->getExposure(deviceId, actualExposureMs))
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider rejected exposure")));
                return;
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("exposureMs"), actualExposureMs);
            reply(response);
            return;
        }

        if (type == driverhost::kCommandSetRoi
            || type == driverhost::kCommandClearRoi
            || type == driverhost::kCommandGetRoi)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            if (type == driverhost::kCommandSetRoi)
            {
                const bool ok = m_camera->setROI(deviceId,
                                                 message.value(QStringLiteral("x")).toInt(),
                                                 message.value(QStringLiteral("y")).toInt(),
                                                 message.value(QStringLiteral("width")).toInt(),
                                                 message.value(QStringLiteral("height")).toInt());
                reply(ok ? makeResponse(type, requestId, true)
                         : makeErrorResponse(type,
                                             requestId,
                                             QStringLiteral("Provider rejected ROI")));
                return;
            }
            if (type == driverhost::kCommandClearRoi)
            {
                const bool ok = m_camera->clearROI(deviceId);
                reply(ok ? makeResponse(type, requestId, true)
                         : makeErrorResponse(type,
                                             requestId,
                                             QStringLiteral("Provider rejected ROI reset")));
                return;
            }

            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (!m_camera->getROI(deviceId, x, y, width, height))
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider failed to read ROI")));
                return;
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("x"), x);
            response.insert(QStringLiteral("y"), y);
            response.insert(QStringLiteral("width"), width);
            response.insert(QStringLiteral("height"), height);
            reply(response);
            return;
        }

        if (type == driverhost::kCommandCaptureEvent)
        {
            m_eventSequences.remove(deviceId);
            if (!supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            ImageFrame frame;
            quint64 frameIndex = 0;
            const auto transport = cameraTransport(deviceId);
            const int timeoutMs = (std::max)(1,
                message.value(QStringLiteral("timeoutMs")).toInt(1500));
            const auto writeEventFrame = [&]()
            {
                QMutexLocker locker(&transport->writeMutex);
                return writeFrame(*transport, frame, &frameIndex);
            };
            if (!transport
                || !m_camera->captureEventFrame(deviceId, frame, timeoutMs)
                || !writeEventFrame())
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider failed to capture a frame")));
                return;
            }
            emitFrameAvailableEvent(deviceId, frameIndex);
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
            reply(response);
            return;
        }

        if (type == driverhost::kCommandCaptureEventSequence
            || type == driverhost::kCommandSendEventFrames
            || type == driverhost::kCommandGetPixelSize)
        {
            const auto transport = cameraTransport(deviceId);
            if (!transport || !supportsCapability(*device, driverhost::kCapabilityCamera))
            {
                unsupported(driverhost::kCapabilityCamera);
                return;
            }
            if (type == driverhost::kCommandGetPixelSize)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(driverhost::kPixelSizeField, m_camera->pixelSizeUm(deviceId));
                reply(response);
                return;
            }
            if (type == driverhost::kCommandCaptureEventSequence)
            {
                std::vector<ImageFrame> frames;
                const int count = message.value(QStringLiteral("count")).toInt();
                const int timeoutMs = (std::max)(1, message.value(QStringLiteral("timeoutMs")).toInt(10000));
                if (!m_camera->captureEventSequence(deviceId, count, frames, timeoutMs)
                    || static_cast<int>(frames.size()) != count)
                {
                    reply(makeErrorResponse(type, requestId,
                                            QStringLiteral("Provider failed to capture an image sequence")));
                    return;
                }
                m_eventSequences.insert(deviceId, std::move(frames));
                reply(makeResponse(type, requestId, true));
                return;
            }
            const int first = message.value(QStringLiteral("first")).toInt();
            const int count = message.value(QStringLiteral("count")).toInt();
            const auto frames = m_eventSequences.find(deviceId);
            if (frames == m_eventSequences.end() || first < 0 || count < 1
                || first + count > static_cast<int>(frames->size()))
            {
                reply(makeErrorResponse(type, requestId, QStringLiteral("No such images of the sequence")));
                return;
            }
            {
                QMutexLocker locker(&transport->writeMutex);
                for (int index = first; index < first + count; ++index)
                {
                    quint64 frameIndex = 0;
                    if (!writeFrame(*transport, (*frames)[static_cast<std::size_t>(index)], &frameIndex))
                    {
                        reply(makeErrorResponse(type, requestId,
                                                QStringLiteral("Failed to write an image of the sequence")));
                        return;
                    }
                }
            }
            if (first + count == static_cast<int>(frames->size()))
            {
                m_eventSequences.erase(frames);
            }
            reply(makeResponse(type, requestId, true));
            return;
        }

        if (type == driverhost::kCommandGetPropertySequenceCapacity
            || type == driverhost::kCommandStartPropertySequence
            || type == driverhost::kCommandStopPropertySequence)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityProperties))
            {
                unsupported(driverhost::kCapabilityProperties);
                return;
            }
            const QString name = message.value(QStringLiteral("name")).toString();
            if (type == driverhost::kCommandGetPropertySequenceCapacity)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("capacity"),
                                static_cast<double>(m_properties->propertySequenceCapacity(deviceId, name)));
                reply(response);
                return;
            }
            if (type == driverhost::kCommandStopPropertySequence)
            {
                m_properties->stopPropertySequence(deviceId, name);
                reply(makeResponse(type, requestId, true));
                return;
            }
            QStringList values;
            for (const QJsonValue& value : message.value(QStringLiteral("values")).toArray())
            {
                values.append(value.toString());
            }
            QString error;
            const bool ok = m_properties->startPropertySequence(deviceId, name, values, &error);
            reply(ok ? makeResponse(type, requestId, true) : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandGetStageSequenceCapacity
            || type == driverhost::kCommandStartStageSequence
            || type == driverhost::kCommandStopStageSequence)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityStage))
            {
                unsupported(driverhost::kCapabilityStage);
                return;
            }
            if (type == driverhost::kCommandGetStageSequenceCapacity)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("capacity"),
                                static_cast<double>(m_stage->stageSequenceCapacity(deviceId)));
                reply(response);
                return;
            }
            if (type == driverhost::kCommandStopStageSequence)
            {
                m_stage->stopStageSequence(deviceId);
                reply(makeResponse(type, requestId, true));
                return;
            }
            std::vector<double> positions;
            for (const QJsonValue& value : message.value(QStringLiteral("positions")).toArray())
            {
                positions.push_back(value.toDouble());
            }
            QString error;
            const bool ok = m_stage->startStageSequence(deviceId, positions, &error);
            reply(ok ? makeResponse(type, requestId, true) : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandRunAutofocus)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityAutofocus))
            {
                unsupported(driverhost::kCapabilityAutofocus);
                return;
            }
            QString error;
            const bool ok = m_autofocus->runAutofocus(deviceId, &error);
            reply(ok ? makeResponse(type, requestId, true) : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandListProperties
            || type == driverhost::kCommandGetProperty
            || type == driverhost::kCommandSetProperty)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityProperties))
            {
                unsupported(driverhost::kCapabilityProperties);
                return;
            }
            if (type == driverhost::kCommandListProperties)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("properties"),
                                QJsonArray::fromStringList(
                                    m_properties->listProperties(deviceId)));
                reply(response);
                return;
            }

            const QString name = message.value(QStringLiteral("name")).toString();
            if (type == driverhost::kCommandSetProperty)
            {
                QString error;
                const bool ok = m_properties->setProperty(
                    deviceId,
                    name,
                    message.value(QStringLiteral("value")).toString(),
                    &error);
                reply(ok ? makeResponse(type, requestId, true)
                         : makeErrorResponse(type, requestId, error));
                return;
            }

            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("value"),
                            m_properties->getProperty(
                                deviceId,
                                name,
                                message.value(QStringLiteral("fromCache")).toBool(false)));
            response.insert(QStringLiteral("propertyType"),
                            m_properties->getPropertyType(deviceId, name));
            response.insert(QStringLiteral("readOnly"),
                            m_properties->isPropertyReadOnly(deviceId, name));
            response.insert(QStringLiteral("preInit"),
                            m_properties->isPropertyPreInit(deviceId, name));
            response.insert(QStringLiteral("allowedValues"),
                            QJsonArray::fromStringList(
                                m_properties->getAllowedPropertyValues(deviceId, name)));
            const bool hasLimits = m_properties->hasPropertyLimits(deviceId, name);
            response.insert(QStringLiteral("hasLimits"), hasLimits);
            response.insert(QStringLiteral("lowerLimit"),
                            hasLimits
                                ? m_properties->getPropertyLowerLimit(deviceId, name)
                                : 0.0);
            response.insert(QStringLiteral("upperLimit"),
                            hasLimits
                                ? m_properties->getPropertyUpperLimit(deviceId, name)
                                : 0.0);
            reply(response);
            return;
        }

        if (type == driverhost::kCommandGetXYPosition
            || type == driverhost::kCommandGetZPosition
            || type == driverhost::kCommandSetRelativeXYPosition
            || type == driverhost::kCommandSetRelativeZPosition
            || type == driverhost::kCommandSetXYPosition
            || type == driverhost::kCommandSetZPosition)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityStage))
            {
                unsupported(driverhost::kCapabilityStage);
                return;
            }
            QString error;
            if (type == driverhost::kCommandGetXYPosition)
            {
                double x = 0.0;
                double y = 0.0;
                if (!m_stage->getXYPosition(deviceId, x, y, &error))
                {
                    reply(makeErrorResponse(type, requestId, error));
                    return;
                }
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("x"), x);
                response.insert(QStringLiteral("y"), y);
                reply(response);
                return;
            }
            if (type == driverhost::kCommandGetZPosition)
            {
                double z = 0.0;
                if (!m_stage->getZPosition(deviceId, z, &error))
                {
                    reply(makeErrorResponse(type, requestId, error));
                    return;
                }
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("z"), z);
                reply(response);
                return;
            }

            bool ok = false;
            if (type == driverhost::kCommandSetRelativeXYPosition)
            {
                ok = m_stage->setRelativeXYPosition(
                    deviceId,
                    message.value(QStringLiteral("x")).toDouble(),
                    message.value(QStringLiteral("y")).toDouble(),
                    &error);
            }
            else if (type == driverhost::kCommandSetRelativeZPosition)
            {
                ok = m_stage->setRelativeZPosition(
                    deviceId,
                    message.value(QStringLiteral("z")).toDouble(),
                    &error);
            }
            else if (type == driverhost::kCommandSetXYPosition)
            {
                ok = m_stage->setXYPosition(
                    deviceId,
                    message.value(QStringLiteral("x")).toDouble(),
                    message.value(QStringLiteral("y")).toDouble(),
                    &error);
            }
            else
            {
                ok = m_stage->setZPosition(
                    deviceId,
                    message.value(QStringLiteral("z")).toDouble(),
                    &error);
            }
            reply(ok ? makeResponse(type, requestId, true)
                     : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandGetShutterOpen
            || type == driverhost::kCommandSetShutterOpen)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityShutter))
            {
                unsupported(driverhost::kCapabilityShutter);
                return;
            }
            QString error;
            if (type == driverhost::kCommandGetShutterOpen)
            {
                bool open = false;
                if (!m_shutter->isShutterOpen(deviceId, open, &error))
                {
                    reply(makeErrorResponse(type, requestId, error));
                    return;
                }
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("open"), open);
                reply(response);
                return;
            }
            const bool ok = m_shutter->setShutterOpen(
                deviceId,
                message.value(QStringLiteral("open")).toBool(),
                &error);
            reply(ok ? makeResponse(type, requestId, true)
                     : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandGetState
            || type == driverhost::kCommandSetState
            || type == driverhost::kCommandGetStateLabel)
        {
            if (!supportsCapability(*device, driverhost::kCapabilityState))
            {
                unsupported(driverhost::kCapabilityState);
                return;
            }
            const long requestedState = static_cast<long>(
                message.value(QStringLiteral("state")).toDouble());
            if (type == driverhost::kCommandGetStateLabel)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("label"),
                                m_state->stateLabel(deviceId, requestedState));
                reply(response);
                return;
            }
            QString error;
            if (type == driverhost::kCommandSetState)
            {
                const bool ok = m_state->setState(deviceId, requestedState, &error);
                reply(ok ? makeResponse(type, requestId, true)
                         : makeErrorResponse(type, requestId, error));
                return;
            }
            long state = 0;
            if (!m_state->getState(deviceId, state, &error))
            {
                reply(makeErrorResponse(type, requestId, error));
                return;
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("state"), static_cast<double>(state));
            response.insert(QStringLiteral("label"), m_state->stateLabel(deviceId, state));
            reply(response);
            return;
        }

        reply(makeErrorResponse(type, requestId, QStringLiteral("Unknown control command")));
    }

    void DriverRuntime::stopForExit()
    {
        m_acceptFrames.store(false, std::memory_order_release);
        m_acceptStreams.store(false, std::memory_order_release);
        if (m_signals)
        {
            for (const SignalSourceDescriptor& source : std::as_const(m_signalSources))
            {
                m_signals->stopSignal(source.id);
            }
            m_signals->setSignalSinks({}, {}, {});
        }
        if (m_daq)
        {
            for (const DaqDeviceDescriptor& daqDevice : std::as_const(m_daqDevices))
            {
                m_daq->stopDaq(daqDevice.id);
            }
            m_daq->setDaqSinks({}, {});
        }
        {
            QMutexLocker locker(&m_streamsMutex);
            m_streams.clear();
        }
        m_signals = nullptr;
        m_daq = nullptr;
        if (m_camera)
        {
            for (const auto& transport : m_cameraTransports)
            {
                if (transport && transport->previewRunning)
                {
                    m_camera->stopPreviewFor(transport->deviceId);
                    transport->previewRunning = false;
                }
            }
            m_camera->setFrameSink({});
        }
        m_camera = nullptr;
        m_microManagerCamera.reset();
        m_properties = nullptr;
        m_stage = nullptr;
        m_shutter = nullptr;
        m_autofocus = nullptr;
        m_state = nullptr;
        m_device.reset();
        {
            QWriteLocker locker(&m_cameraTransportsLock);
            m_cameraTransports.clear();
        }
        m_devicesById.clear();
        m_devices.clear();
        if (m_driverLoader)
        {
            m_driverLoader->unload();
            m_driverLoader.reset();
        }
    }

    class DriverHost final : public QObject
    {
        Q_OBJECT

    public:
        // Create the control server wrapper for one isolated runtime
        DriverHost(QString hostKey,
                   DriverRuntime* runtime,
                   QObject* parent = nullptr)
            : QObject(parent)
              , m_hostKey(std::move(hostKey))
              , m_serverName(driverhost::controlServerName(m_hostKey))
              , m_runtime(runtime)
        {
            qRegisterMetaType<QJsonObject>("QJsonObject");
            qRegisterMetaType<quint64>("quint64");
        }

        // Stop the runtime thread before destruction
        ~DriverHost() override
        {
            stopRuntime();
        }

        bool start();

    private slots:
        void onNewControlConnection();
        void onRuntimeResponse(quint64 connectionId, const QJsonObject& response);
        void broadcastEvent(const QJsonObject& event);
        void onConnectionClosed(quint64 connectionId);

    private:
        void stopRuntime();

        QString m_hostKey;
        QString m_serverName;

        std::unique_ptr<QLocalServer> m_ctrlServer;
        QThread m_runtimeThread;
        DriverRuntime* m_runtime{nullptr};
        quint64 m_nextConnectionId{1};
        QHash<quint64, ControlConnection*> m_connections;
    };

    // Start the runtime thread and local control server
    bool DriverHost::start()
    {
        if (!m_runtime)
        {
            return false;
        }
        m_runtime->moveToThread(&m_runtimeThread);
        connect(&m_runtimeThread, &QThread::finished,
                m_runtime, &QObject::deleteLater);
        connect(m_runtime, &DriverRuntime::responseReady,
                this, &DriverHost::onRuntimeResponse);
        connect(m_runtime, &DriverRuntime::eventReady,
                this, &DriverHost::broadcastEvent);
        connect(m_runtime, &DriverRuntime::shutdownRequested, this,
                []() { QCoreApplication::quit(); });

        m_runtimeThread.start();

        bool initialized = false;
        QString initError;
        QMetaObject::invokeMethod(
            m_runtime,
            [this, &initialized, &initError]()
            {
                initialized = m_runtime->initializeRuntime();
                initError = m_runtime->lastError();
            },
            Qt::BlockingQueuedConnection);

        if (!initialized)
        {
            qCritical().noquote()
                << QString("DriverHost init failed for '%1': %2")
                .arg(m_hostKey, initError);
            stopRuntime();
            return false;
        }

        QLocalServer::removeServer(m_serverName);
        m_ctrlServer = std::make_unique<QLocalServer>(this);
        connect(m_ctrlServer.get(), &QLocalServer::newConnection,
                this, &DriverHost::onNewControlConnection);
        if (!m_ctrlServer->listen(m_serverName))
        {
            qCritical().noquote()
                << QString("DriverHost control server failed to listen on %1")
                .arg(m_serverName);
            stopRuntime();
            return false;
        }

        qInfo().noquote()
            << QString("DriverHost control server listening on %1").arg(m_serverName);
        return true;
    }

    // Accept pending local control connections
    void DriverHost::onNewControlConnection()
    {
        while (m_ctrlServer && m_ctrlServer->hasPendingConnections())
        {
            QLocalSocket* socket = m_ctrlServer->nextPendingConnection();
            if (!socket)
            {
                continue;
            }

            const quint64 connectionId = m_nextConnectionId++;
            auto* connection = new ControlConnection(connectionId, socket, this);
            m_connections.insert(connectionId, connection);

            connect(connection, &ControlConnection::requestReceived,
                    m_runtime, &DriverRuntime::handleRequest,
                    Qt::QueuedConnection);
            connect(connection, &ControlConnection::connectionClosed,
                    this, &DriverHost::onConnectionClosed);

            DriverRuntime* const runtime = m_runtime;
            QMetaObject::invokeMethod(runtime,
                                      [runtime]() { runtime->publishHello(); },
                                      Qt::QueuedConnection);
        }
    }

    // Route one runtime response back to its connection
    void DriverHost::onRuntimeResponse(quint64 connectionId, const QJsonObject& response)
    {
        auto it = m_connections.find(connectionId);
        if (it == m_connections.end() || !it.value())
        {
            return;
        }
        it.value()->sendMessage(response);
    }

    // Broadcast one runtime event to all clients
    void DriverHost::broadcastEvent(const QJsonObject& event)
    {
        for (auto it = m_connections.begin(); it != m_connections.end(); ++it)
        {
            if (it.value())
            {
                it.value()->sendMessage(event);
            }
        }
    }

    // Remove a closed control connection
    void DriverHost::onConnectionClosed(quint64 connectionId)
    {
        m_connections.remove(connectionId);
    }

    // Stop the runtime worker thread cleanly
    void DriverHost::stopRuntime()
    {
        if (!m_runtime)
        {
            return;
        }

        DriverRuntime* const runtime = m_runtime;
        QMetaObject::invokeMethod(runtime,
                                  [runtime]() { runtime->stopForExit(); },
                                  Qt::BlockingQueuedConnection);
        m_runtimeThread.quit();
        m_runtimeThread.wait();
        m_runtime = nullptr;
    }

    // Quit through the normal shutdown path when the launching app dies or a
    // termination signal arrives, so shared memory is released instead of leaked.
    // Called before the runtime starts; events seen meanwhile quit once exec runs.
    // Returns false when the launcher is already gone.
    bool quitWithLauncher(QCoreApplication& app, qint64 launcherPid)
    {
#ifdef Q_OS_WIN
        HANDLE launcher = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(launcherPid));
        if (!launcher || WaitForSingleObject(launcher, 0) == WAIT_OBJECT_0)
        {
            if (launcher) CloseHandle(launcher);
            return false;
        }
        auto* launcherExit = new QWinEventNotifier(launcher, &app);
        QObject::connect(launcherExit, &QWinEventNotifier::activated,
                         &app, []() { QCoreApplication::quit(); });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [launcherExit, launcher]()
        {
            delete launcherExit;
            CloseHandle(launcher);
        });
#else
        // The watch fd turns readable once the launcher exits
#ifdef Q_OS_MACOS
        const int launcherWatch = ::kqueue();
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(launcherPid), EVFILT_PROC, EV_ADD | EV_ONESHOT,
               NOTE_EXIT, 0, nullptr);
        if (::kevent(launcherWatch, &change, 1, nullptr, 0, nullptr) != 0)
        {
            ::close(launcherWatch);
            return false;
        }
#else
        const int launcherWatch = static_cast<int>(::syscall(SYS_pidfd_open,
                                                             static_cast<pid_t>(launcherPid), 0));
        if (launcherWatch < 0)
        {
            return false;
        }
#endif
        auto* launcherExit = new QSocketNotifier(launcherWatch, QSocketNotifier::Read, &app);
        QObject::connect(launcherExit, &QSocketNotifier::activated, &app, [launcherExit]()
        {
            launcherExit->setEnabled(false);
            QCoreApplication::quit();
        });

        static int signalPipe[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe);
        for (const int fd : signalPipe)
        {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        struct sigaction action{};
        action.sa_handler = [](int)
        {
            const char byte = 1;
            [[maybe_unused]] const ssize_t written = ::write(signalPipe[0], &byte, 1);
        };
        sigemptyset(&action.sa_mask);
        for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP})
        {
            ::sigaction(signalNumber, &action, nullptr);
        }
        auto* signalNotifier = new QSocketNotifier(signalPipe[1], QSocketNotifier::Read, &app);
        QObject::connect(signalNotifier, &QSocketNotifier::activated, &app, []()
        {
            char buffer[64];
            while (::read(signalPipe[1], buffer, sizeof(buffer)) > 0)
            {
            }
            QCoreApplication::quit();
        });
#endif
        return true;
    }
} // namespace scopeone::core::internal

// Runs one provider in a process of its own: a driver, or one camera of a Micro-Manager configuration
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("ScopeOne DriverHost");
    parser.addHelpOption();

    QCommandLineOption optProvider(QStringLiteral("provider"),
                                   QStringLiteral("Hardware provider ID"),
                                   QStringLiteral("id"));
    QCommandLineOption optDriver(QStringLiteral("driver"),
                                 QStringLiteral("Driver library path"),
                                 QStringLiteral("path"));
    QCommandLineOption optCamera(QStringLiteral("micromanagerCamera"),
                                 QStringLiteral("JSON setup of a Micro-Manager camera"),
                                 QStringLiteral("json"));
    QCommandLineOption optHostKey(QStringLiteral("hostKey"),
                                  QStringLiteral("DriverHost instance key"),
                                  QStringLiteral("key"));
    QCommandLineOption optDeviceName(QStringLiteral("device"),
                                     QStringLiteral("Device name in the driver, the first one by default"),
                                     QStringLiteral("name"));
    QCommandLineOption optProperties(QStringLiteral("properties"),
                                     QStringLiteral("JSON pre-init properties of the device"),
                                     QStringLiteral("json"));
    QCommandLineOption optLauncherPid(QStringLiteral("launcherPid"),
                                      QStringLiteral("Process ID of the launching app"),
                                      QStringLiteral("pid"));

    parser.addOption(optProvider);
    parser.addOption(optDriver);
    parser.addOption(optCamera);
    parser.addOption(optHostKey);
    parser.addOption(optDeviceName);
    parser.addOption(optProperties);
    parser.addOption(optLauncherPid);
    parser.process(app);

    if (!parser.isSet(optLauncherPid)
        || !scopeone::core::internal::quitWithLauncher(app, parser.value(optLauncherPid).toLongLong()))
    {
        qCritical().noquote() << "Missing or exited launcher: --launcherPid";
        return 2;
    }

    const QString providerId = parser.value(optProvider).trimmed();
    const QString hostKey = parser.value(optHostKey).trimmed();
    const QString driverPath = parser.value(optDriver).trimmed();
    if (providerId.isEmpty() || hostKey.isEmpty() || driverPath.isEmpty() == !parser.isSet(optCamera))
    {
        qCritical().noquote() << "DriverHost requires --provider, --hostKey and either --driver or --micromanagerCamera";
        return 2;
    }

    const auto parseObject = [](const QString& text, QJsonObject& object)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            qCritical().noquote() << QStringLiteral("Invalid JSON argument: %1").arg(parseError.errorString());
            return false;
        }
        object = document.object();
        return true;
    };

    std::optional<scopeone::core::internal::MicroManagerCameraSetup> cameraSetup;
    if (parser.isSet(optCamera))
    {
        QJsonObject object;
        if (!parseObject(parser.value(optCamera), object))
        {
            return 2;
        }
        cameraSetup = scopeone::core::internal::MicroManagerCameraSetup::fromJson(object);
    }
    QJsonObject preInitProperties;
    for (const QString& encodedOptions : parser.values(optProperties))
    {
        QJsonObject object;
        if (!parseObject(encodedOptions, object))
        {
            return 2;
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            preInitProperties.insert(it.key(), it.value());
        }
    }

    auto* runtime = new scopeone::core::internal::DriverRuntime(
        driverPath, std::move(cameraSetup), providerId, hostKey,
        parser.value(optDeviceName).trimmed(), preInitProperties.toVariantMap());
    scopeone::core::internal::DriverHost driverHost(hostKey, runtime);
    if (!driverHost.start())
    {
        return 2;
    }

    return app.exec();
}

#include "DriverHostMain.moc"
