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
#include <QSharedMemory>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "MMCore.h"
#include "internal/AgentProtocol.h"
#include "scopeone/SharedFrame.h"

namespace scopeone::core::internal
{
    static_assert(std::atomic_ref<quint32>::is_always_lock_free,
                  "Shared frame state requires lock-free 32-bit atomics");

    using scopeone::core::SharedFrameHeader;
    using scopeone::core::SharedMemoryControl;
    using scopeone::core::SharedPixelFormat;
    using scopeone::core::computeMaxFrameBytes;
    using scopeone::core::kSharedFrameHeaderSize;
    using scopeone::core::kSharedFrameMaxBytes;
    using scopeone::core::kSharedFrameNumSlots;
    using scopeone::core::kSharedFrameSlotStride;
    using scopeone::core::kSharedMemoryControlSize;

    constexpr int kMinimumPollIntervalMs = 1;
    constexpr int kMaximumPollIntervalMs = 50;
    constexpr int kPreviewFrameDeliveryIntervalMs = 16;

    int pollingIntervalFor(double frameIntervalMs)
    {
        if (!std::isfinite(frameIntervalMs) || frameIntervalMs <= 0.0)
        {
            return kMinimumPollIntervalMs;
        }
        return static_cast<int>(std::clamp(frameIntervalMs / 4.0,
                                           static_cast<double>(kMinimumPollIntervalMs),
                                           static_cast<double>(kMaximumPollIntervalMs)));
    }

    // Normalize frame bit depth before publishing shared frame metadata
    static quint16 normalizedSharedBitDepth(SharedPixelFormat format, int bitsPerSample)
    {
        if (format == SharedPixelFormat::Mono8)
        {
            return 8;
        }
        if (format == SharedPixelFormat::Mono16)
        {
            return bitsPerSample >= 1 && bitsPerSample <= 16
                       ? static_cast<quint16>(bitsPerSample)
                       : 16;
        }
        return 0;
    }

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
            if (!socket)
            {
                qFatal("ControlConnection requires QLocalSocket");
            }
            m_socket->setParent(this);

            connect(m_socket, &QLocalSocket::readyRead,
                    this, &ControlConnection::onReadyRead);
            connect(m_socket, &QLocalSocket::disconnected,
                    this, &ControlConnection::onDisconnected);
            connect(m_socket, &QLocalSocket::errorOccurred, this,
                    [this](QLocalSocket::LocalSocketError socketError)
                    {
                        qWarning().noquote()
                            << QString("Agent control socket error (%1) on connection %2")
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
            m_socket->write(agent::encodeMessage(message));
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
                const agent::DecodeResult result =
                    agent::tryDecodeMessage(m_readBuffer, message, &error);
                if (result == agent::DecodeResult::Incomplete)
                {
                    return;
                }
                if (result == agent::DecodeResult::Error)
                {
                    qWarning().noquote()
                        << QString("Agent control protocol error on connection %1: %2")
                           .arg(m_connectionId)
                           .arg(error);
                    m_socket->disconnectFromServer();
                    return;
                }

                if (message.value(agent::kEnvelopeVersionField).toInt(0)
                    != static_cast<int>(agent::kProtocolVersion))
                {
                    qWarning().noquote()
                        << QString("Agent control protocol version mismatch on connection %1")
                        .arg(m_connectionId);
                    m_socket->disconnectFromServer();
                    return;
                }

                if (message.value(agent::kEnvelopeKindField).toString()
                    != agent::kMessageKindRequest)
                {
                    qWarning().noquote()
                        << QString("Ignoring non-request control message on connection %1")
                        .arg(m_connectionId);
                    continue;
                }

                const quint64 requestId =
                    agent::decodeUInt64(message.value(agent::kEnvelopeRequestIdField));
                const QString type = message.value(agent::kMessageTypeField).toString();
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

        // Notify the agent when this connection closes
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

    class AgentRuntime final : public QObject
    {
        Q_OBJECT

    public:
        // Store launch settings for one camera runtime
        AgentRuntime(QString cameraId,
                     QString adapter,
                     QString device,
                     QString shmKey,
                     QStringList preInitProperties,
                     QStringList properties,
                     double exposureMs,
                     bool autoPreview,
                     QObject* parent = nullptr)
            : QObject(parent)
              , m_cameraId(std::move(cameraId))
              , m_adapter(std::move(adapter))
              , m_device(std::move(device))
              , m_shmKey(std::move(shmKey))
              , m_preInitProperties(std::move(preInitProperties))
              , m_properties(std::move(properties))
              , m_exposureMs(exposureMs)
              , m_autoPreview(autoPreview)
        {
        }

        QString lastError() const
        {
            return m_lastError;
        }

        // Initialize MMCore camera state and shared memory transport
        bool initializeRuntime()
        {
            m_lastError.clear();
            setState(State::Starting);

            if (!m_timer)
            {
                m_timer = new QTimer(this);
                m_timer->setTimerType(Qt::PreciseTimer);
                connect(m_timer, &QTimer::timeout,
                        this, &AgentRuntime::pollAndWrite);
            }

            try
            {
                m_mmcore = std::make_unique<CMMCore>();
                const std::string label = m_cameraId.toStdString();
                const std::string adapter = m_adapter.toStdString();
                const std::string device = m_device.toStdString();

                m_mmcore->loadDevice(label.c_str(), adapter.c_str(), device.c_str());
                QString preInitError;
                if (!applyProperties(m_preInitProperties, label, QStringLiteral("pre-init property"), &preInitError))
                {
                    m_lastError = preInitError;
                    setState(State::Error, m_lastError);
                    return false;
                }
                m_mmcore->initializeDevice(label.c_str());
                m_mmcore->setCameraDevice(label.c_str());

                QString propertyError;
                if (!applyProperties(m_properties, label, QStringLiteral("property"), &propertyError))
                {
                    m_lastError = propertyError;
                    setState(State::Error, m_lastError);
                    return false;
                }

                try
                {
                    m_mmcore->setCircularBufferMemoryFootprint(2048);
                }
                catch (const CMMError&)
                {
                }

                double finalExposure = 0.0;
                if (m_exposureMs > 0.0)
                {
                    m_mmcore->setExposure(m_exposureMs);
                    finalExposure = m_mmcore->getExposure();
                }
                else
                {
                    finalExposure = m_mmcore->getExposure();
                    if (finalExposure <= 0.0)
                    {
                        m_lastError = QStringLiteral("Camera reported an invalid exposure");
                        setState(State::Error, m_lastError);
                        return false;
                    }
                }
                m_exposureMs = finalExposure;
            }
            catch (const CMMError& error)
            {
                m_lastError = QString::fromStdString(error.getMsg());
                setState(State::Error, m_lastError);
                return false;
            }

            m_shm = std::make_unique<QSharedMemory>();
            m_shm->setNativeKey(m_shmKey);
            const int totalBytes =
                kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
            if (!m_shm->create(totalBytes))
            {
                if (m_shm->attach())
                {
                    m_shm->detach();
                }
                if (!m_shm->create(totalBytes))
                {
                    m_lastError = QStringLiteral("Cannot create shared memory '%1'")
                        .arg(m_shmKey);
                    setState(State::Error, m_lastError);
                    return false;
                }
            }

            if (m_shm->lock())
            {
                auto* base = static_cast<uchar*>(m_shm->data());
                if (base)
                {
                    const SharedMemoryControl control{};
                    memcpy(base, &control, sizeof(control));
                    for (int i = 0; i < kSharedFrameNumSlots; ++i)
                    {
                        const SharedFrameHeader header{};
                        uchar* slot = base + kSharedMemoryControlSize
                                      + i * kSharedFrameSlotStride;
                        memcpy(slot, &header, sizeof(header));
                    }
                }
                m_shm->unlock();
            }

            if (m_autoPreview)
            {
                QString error;
                if (!startPreviewInternal(&error))
                {
                    m_lastError = error;
                    setState(State::Error, m_lastError);
                    return false;
                }
            }
            else
            {
                setState(State::Idle);
            }

            return true;
        }

    public slots:
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
        // Runtime state is mirrored to control clients
        enum class State
        {
            Starting,
            Idle,
            Previewing,
            Error,
            ShuttingDown
        };

        enum class FrameDeliveryMode
        {
            PreviewLatest,
            LatestOnly,
            AllFrames
        };

        // Frame layout describes one shared memory payload shape
        struct FrameLayout
        {
            unsigned width{0};
            unsigned height{0};
            unsigned bytesPerPixel{0};
            SharedPixelFormat format{SharedPixelFormat::Mono8};
            unsigned stride{0};
            quint64 byteCount{0};
            quint16 bitDepth{8};
            quint16 channels{1};
        };

        bool previewRunning() const;
        void setState(State state, const QString& error = QString());
        QJsonObject makeResponse(const QString& type, quint64 requestId, bool ok) const;
        QJsonObject makeErrorResponse(const QString& type,
                                      quint64 requestId,
                                      const QString& error) const;
        QJsonObject makeEvent(const QString& type) const;
        bool applyProperties(const QStringList& encodedProperties,
                             const std::string& label,
                             const QString& propertyKind,
                             QString* errorMessage);
        void emitPreviewStateEvent();
        void emitAgentErrorEvent(const QString& error);
        void emitFrameAvailableEvent(quint64 frameIndex);
        bool startPreviewInternal(QString* errorMessage);
        bool stopPreviewInternal(QString* errorMessage);
        bool captureEventFrameInternal(quint64& frameIndex, QString* errorMessage);
        bool writeFrameToSharedMemory(const void* pixels,
                                      quint64 frameAdvance = 1,
                                      quint64* frameIndexOut = nullptr);
        void pollAndWrite();
        void updatePollingInterval(quint64 frameCount);
        bool ensureFrameLayout(unsigned width, unsigned height, unsigned bytesPerPixel);
        void refreshSourceRoi();

        QString m_cameraId;
        QString m_adapter;
        QString m_device;
        QString m_shmKey;
        QStringList m_preInitProperties;
        QStringList m_properties;
        double m_exposureMs{10.0};
        bool m_autoPreview{false};

        QString m_lastError;
        State m_state{State::Starting};

        std::unique_ptr<CMMCore> m_mmcore;
        std::unique_ptr<QSharedMemory> m_shm;
        QTimer* m_timer{nullptr};
        QElapsedTimer m_frameIntervalTimer;
        QElapsedTimer m_deliveryTimer;
        double m_observedFrameIntervalMs{0.0};

        FrameLayout m_frameLayout{};
        bool m_frameLayoutValid{false};
        bool m_loggedOversizedFrame{false};
        bool m_loggedUnsupportedFormat{false};
        int m_sourceRoiX{0};
        int m_sourceRoiY{0};
        int m_sourceRoiWidth{0};
        int m_sourceRoiHeight{0};

        quint64 m_frameIndex{0};
        quint64 m_pendingFrameAdvance{0};
        int m_nextWriteSlot{0};
        FrameDeliveryMode m_frameDeliveryMode{FrameDeliveryMode::PreviewLatest};
    };

    // Publish the initial hello event to connected clients
    void AgentRuntime::publishHello()
    {
        emit eventReady(makeEvent(agent::kEventHello));
    }

    // Replay encoded cfg properties into the agent MMCore instance
    bool AgentRuntime::applyProperties(const QStringList& encodedProperties,
                                       const std::string& label,
                                       const QString& propertyKind,
                                       QString* errorMessage)
    {
        for (const QString& encodedProperty : encodedProperties)
        {
            if (encodedProperty.isEmpty())
            {
                continue;
            }

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(encodedProperty.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Invalid %1 payload for '%2'")
                        .arg(propertyKind, m_cameraId);
                }
                return false;
            }

            const QJsonObject property = doc.object();
            const QString propertyName = property.value(QStringLiteral("name")).toString().trimmed();
            const QString propertyValue = property.value(QStringLiteral("value")).toString();
            if (propertyName.isEmpty())
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Missing %1 name for '%2'")
                        .arg(propertyKind, m_cameraId);
                }
                return false;
            }

            try
            {
                m_mmcore->setProperty(label.c_str(),
                                      propertyName.toStdString().c_str(),
                                      propertyValue.toStdString().c_str());
            }
            catch (const CMMError& mmError)
            {
                if (errorMessage)
                {
                    *errorMessage = QString("Failed to apply %1 '%2': %3")
                        .arg(propertyKind, propertyName, QString::fromStdString(mmError.getMsg()));
                }
                return false;
            }
        }

        return true;
    }

    // Dispatch one control request and emit a matching response
    void AgentRuntime::handleRequest(quint64 connectionId,
                                     quint64 requestId,
                                     const QString& type,
                                     const QJsonObject& message)
    {
        if (requestId == 0 || type.isEmpty())
        {
            return;
        }

        if (!m_mmcore && type != agent::kCommandShutdown)
        {
            emit responseReady(connectionId,
                               makeErrorResponse(type,
                                                 requestId,
                                                 QStringLiteral("Agent runtime not initialized")));
            return;
        }

        if (type == agent::kCommandStartPreview)
        {
            QString error;
            const bool ok = startPreviewInternal(&error);
            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == agent::kCommandStopPreview)
        {
            QString error;
            const bool ok = stopPreviewInternal(&error);
            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == agent::kCommandSetFrameDeliveryMode)
        {
            const QString mode = message.value(QStringLiteral("mode")).toString();
            if (mode == agent::kFrameDeliveryModePreviewLatest)
            {
                m_frameDeliveryMode = FrameDeliveryMode::PreviewLatest;
                m_deliveryTimer.invalidate();
            }
            else if (mode == agent::kFrameDeliveryModeLatestOnly)
            {
                m_frameDeliveryMode = FrameDeliveryMode::LatestOnly;
            }
            else if (mode == agent::kFrameDeliveryModeAllFrames)
            {
                m_frameDeliveryMode = FrameDeliveryMode::AllFrames;
                m_pendingFrameAdvance = 0;
                m_deliveryTimer.invalidate();
            }
            else
            {
                emit responseReady(connectionId,
                                   makeErrorResponse(type,
                                                     requestId,
                                                     QStringLiteral("Unknown frame delivery mode")));
                return;
            }

            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("mode"), mode);
            emit responseReady(connectionId, response);
            return;
        }

        if (type == agent::kCommandSetExposure)
        {
            const double exposureMs = message.value(QStringLiteral("value")).toDouble(-1.0);
            bool ok = exposureMs > 0.0;
            QString error;
            if (!ok)
            {
                error = QStringLiteral("Invalid exposure value");
            }
            else
            {
                try
                {
                    m_mmcore->setExposure(exposureMs);
                    m_exposureMs = m_mmcore->getExposure();
                }
                catch (const CMMError& mmError)
                {
                    ok = false;
                    error = QString::fromStdString(mmError.getMsg());
                }
            }

            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("exposureMs"), m_exposureMs);
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == agent::kCommandListProperties)
        {
            QJsonArray properties;
            QString error;
            bool ok = true;
            try
            {
                const auto names =
                    m_mmcore->getDevicePropertyNames(m_cameraId.toStdString().c_str());
                for (const auto& name : names)
                {
                    properties.append(QString::fromStdString(name));
                }
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("properties"), properties);
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == agent::kCommandGetProperty)
        {
            const QString name = message.value(QStringLiteral("name")).toString();
            const bool fromCache = message.value(QStringLiteral("fromCache")).toBool(false);
            const std::string camera = m_cameraId.toStdString();
            const std::string property = name.toStdString();
            QString value;
            QString propertyType = QStringLiteral("Unknown");
            bool readOnly = true;
            bool preInit = false;
            QJsonArray allowedValues;
            bool hasLimits = false;
            double lowerLimit = 0.0;
            double upperLimit = 0.0;
            QString error;
            bool ok = true;

            try
            {
                value = QString::fromStdString(
                    fromCache
                        ? m_mmcore->getPropertyFromCache(camera.c_str(), property.c_str())
                        : m_mmcore->getProperty(camera.c_str(), property.c_str()));

                try
                {
                    switch (m_mmcore->getPropertyType(camera.c_str(), property.c_str()))
                    {
                    case MM::String:
                        propertyType = QStringLiteral("String");
                        break;
                    case MM::Float:
                        propertyType = QStringLiteral("Float");
                        break;
                    case MM::Integer:
                        propertyType = QStringLiteral("Integer");
                        break;
                    default:
                        propertyType = QStringLiteral("Unknown");
                        break;
                    }
                }
                catch (const CMMError&)
                {
                }

                try
                {
                    preInit = m_mmcore->isPropertyPreInit(camera.c_str(), property.c_str());
                }
                catch (const CMMError&)
                {
                }

                try
                {
                    readOnly = m_mmcore->isPropertyReadOnly(camera.c_str(), property.c_str());
                }
                catch (const CMMError&)
                {
                }

                try
                {
                    const auto values =
                        m_mmcore->getAllowedPropertyValues(camera.c_str(), property.c_str());
                    for (const auto& allowedValue : values)
                    {
                        allowedValues.append(QString::fromStdString(allowedValue));
                    }
                }
                catch (const CMMError&)
                {
                }

                try
                {
                    hasLimits = m_mmcore->hasPropertyLimits(camera.c_str(), property.c_str());
                    if (hasLimits)
                    {
                        lowerLimit = m_mmcore->getPropertyLowerLimit(camera.c_str(), property.c_str());
                        upperLimit = m_mmcore->getPropertyUpperLimit(camera.c_str(), property.c_str());
                    }
                }
                catch (const CMMError&)
                {
                    hasLimits = false;
                }
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("value"), value);
                response.insert(QStringLiteral("propertyType"), propertyType);
                response.insert(QStringLiteral("readOnly"), readOnly);
                response.insert(QStringLiteral("preInit"), preInit);
                response.insert(QStringLiteral("allowedValues"), allowedValues);
                response.insert(QStringLiteral("hasLimits"), hasLimits);
                response.insert(QStringLiteral("lowerLimit"), lowerLimit);
                response.insert(QStringLiteral("upperLimit"), upperLimit);
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == agent::kCommandSetProperty)
        {
            const QString name = message.value(QStringLiteral("name")).toString();
            const QString value = message.value(QStringLiteral("value")).toString();
            QString error;
            bool ok = true;
            try
            {
                m_mmcore->setProperty(m_cameraId.toStdString().c_str(),
                                      name.toStdString().c_str(),
                                      value.toStdString().c_str());
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == agent::kCommandSetRoi)
        {
            const int x = message.value(QStringLiteral("x")).toInt(0);
            const int y = message.value(QStringLiteral("y")).toInt(0);
            const int width = message.value(QStringLiteral("width")).toInt(0);
            const int height = message.value(QStringLiteral("height")).toInt(0);
            QString error;
            bool ok = true;
            try
            {
                m_mmcore->setROI(m_cameraId.toStdString().c_str(), x, y, width, height);
                m_mmcore->waitForDevice(m_cameraId.toStdString().c_str());
                refreshSourceRoi();
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == agent::kCommandClearRoi)
        {
            QString error;
            bool ok = true;
            try
            {
                m_mmcore->setCameraDevice(m_cameraId.toStdString().c_str());
                m_mmcore->clearROI();
                m_mmcore->waitForDevice(m_cameraId.toStdString().c_str());
                refreshSourceRoi();
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == agent::kCommandGetRoi)
        {
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            QString error;
            bool ok = true;
            try
            {
                m_mmcore->getROI(m_cameraId.toStdString().c_str(), x, y, width, height);
            }
            catch (const CMMError& mmError)
            {
                ok = false;
                error = QString::fromStdString(mmError.getMsg());
            }

            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("x"), x);
                response.insert(QStringLiteral("y"), y);
                response.insert(QStringLiteral("width"), width);
                response.insert(QStringLiteral("height"), height);
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == agent::kCommandCaptureEvent)
        {
            QString error;
            quint64 frameIndex = 0;
            const bool ok = captureEventFrameInternal(frameIndex, &error);
            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("frameIndex"), agent::encodeUInt64(frameIndex));
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == agent::kCommandShutdown)
        {
            stopPreviewInternal(nullptr);
            setState(State::ShuttingDown);
            emit responseReady(connectionId, makeResponse(type, requestId, true));
            QTimer::singleShot(50, this, [this]() { emit shutdownRequested(); });
            return;
        }

        emit responseReady(connectionId,
                           makeErrorResponse(type,
                                             requestId,
                                             QStringLiteral("Unknown control command")));
    }

    // Stop preview before the runtime thread exits
    void AgentRuntime::stopForExit()
    {
        stopPreviewInternal(nullptr);
        setState(State::ShuttingDown);
    }

    // Check whether the runtime is currently previewing
    bool AgentRuntime::previewRunning() const
    {
        return m_state == State::Previewing;
    }

    // Update runtime state and publish state changes
    void AgentRuntime::setState(State state, const QString& error)
    {
        const bool changed = (m_state != state);
        m_state = state;
        if (!error.isEmpty())
        {
            m_lastError = error;
        }
        if (changed && (state == State::Previewing
            || state == State::Idle
            || state == State::Error
            || state == State::ShuttingDown))
        {
            emitPreviewStateEvent();
        }
        if (!error.isEmpty())
        {
            emitAgentErrorEvent(error);
        }
    }

    // Build a protocol response envelope
    QJsonObject AgentRuntime::makeResponse(const QString& type, quint64 requestId, bool ok) const
    {
        QJsonObject response =
            agent::makeEnvelope(agent::kMessageKindResponse, type, requestId);
        response.insert(QStringLiteral("ok"), ok);
        return response;
    }

    // Build a protocol error response envelope
    QJsonObject AgentRuntime::makeErrorResponse(const QString& type,
                                                quint64 requestId,
                                                const QString& error) const
    {
        QJsonObject response = makeResponse(type, requestId, false);
        response.insert(QStringLiteral("error"), error);
        return response;
    }

    // Build a protocol event envelope
    QJsonObject AgentRuntime::makeEvent(const QString& type) const
    {
        return agent::makeEnvelope(agent::kMessageKindEvent, type);
    }

    // Publish current preview state to clients
    void AgentRuntime::emitPreviewStateEvent()
    {
        QJsonObject event = makeEvent(agent::kEventPreviewState);
        event.insert(QStringLiteral("running"), previewRunning());
        emit eventReady(event);
    }

    // Publish an agent error event to clients
    void AgentRuntime::emitAgentErrorEvent(const QString& error)
    {
        QJsonObject event = makeEvent(agent::kEventAgentError);
        event.insert(QStringLiteral("error"), error);
        emit eventReady(event);
    }

    // Publish the newest shared memory frame index
    void AgentRuntime::emitFrameAvailableEvent(quint64 frameIndex)
    {
        QJsonObject event = makeEvent(agent::kEventFrameAvailable);
        event.insert(QStringLiteral("frameIndex"), agent::encodeUInt64(frameIndex));
        emit eventReady(event);
    }

    // Start continuous acquisition and polling
    bool AgentRuntime::startPreviewInternal(QString* errorMessage)
    {
        if (!m_mmcore)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        if (m_state == State::ShuttingDown || m_state == State::Error)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Agent is not in a runnable state");
            }
            return false;
        }
        if (previewRunning())
        {
            return true;
        }

        try
        {
            while (m_mmcore->getRemainingImageCount() > 0)
            {
                m_mmcore->popNextImage();
            }
            refreshSourceRoi();
            if (!ensureFrameLayout(m_mmcore->getImageWidth(),
                                   m_mmcore->getImageHeight(),
                                   m_mmcore->getBytesPerPixel()))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unsupported frame format");
                }
                return false;
            }
            m_mmcore->startContinuousSequenceAcquisition(0.0);
            m_observedFrameIntervalMs = 0.0;
            m_pendingFrameAdvance = 0;
            m_frameIntervalTimer.restart();
            m_deliveryTimer.invalidate();
            m_timer->setInterval(pollingIntervalFor(m_exposureMs));
            if (m_timer && !m_timer->isActive())
            {
                m_timer->start();
            }
            setState(State::Previewing);
            return true;
        }
        catch (const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
            }
            return false;
        }
    }

    // Stop continuous acquisition and polling
    bool AgentRuntime::stopPreviewInternal(QString* errorMessage)
    {
        if (!m_mmcore)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }

        try
        {
            if (m_mmcore->isSequenceRunning())
            {
                m_mmcore->stopSequenceAcquisition();
            }
            if (m_timer && m_timer->isActive())
            {
                m_timer->stop();
            }
            m_frameIntervalTimer.invalidate();
            m_deliveryTimer.invalidate();
            m_observedFrameIntervalMs = 0.0;
            m_pendingFrameAdvance = 0;
            if (m_state != State::ShuttingDown && m_state != State::Error)
            {
                setState(State::Idle);
            }
            return true;
        }
        catch (const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
            }
            return false;
        }
    }

    // Capture one frame for recording or API requests
    bool AgentRuntime::captureEventFrameInternal(quint64& frameIndex, QString* errorMessage)
    {
        frameIndex = 0;
        try
        {
            const void* pixels = nullptr;
            if (previewRunning())
            {
                QElapsedTimer waitTimer;
                waitTimer.start();
                while (m_mmcore->getRemainingImageCount() <= 0 && waitTimer.elapsed() < 2000)
                {
                    QThread::msleep(1);
                }
                if (m_mmcore->getRemainingImageCount() > 0)
                {
                    pixels = m_mmcore->popNextImage();
                }
                else if (errorMessage)
                {
                    *errorMessage = QStringLiteral("No frame available from running sequence");
                }
            }
            else
            {
                m_mmcore->snapImage();
                pixels = m_mmcore->getImage();
            }

            if (!pixels)
            {
                if (errorMessage && errorMessage->isEmpty())
                {
                    *errorMessage = QStringLiteral("Empty image buffer");
                }
                return false;
            }

            const unsigned width = m_mmcore->getImageWidth();
            const unsigned height = m_mmcore->getImageHeight();
            const unsigned bytesPerPixel = m_mmcore->getBytesPerPixel();
            if (!ensureFrameLayout(width, height, bytesPerPixel))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unsupported frame format");
                }
                return false;
            }

            const quint64 frameAdvance = m_pendingFrameAdvance + 1;
            m_pendingFrameAdvance = 0;
            if (!writeFrameToSharedMemory(pixels, frameAdvance, &frameIndex))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Shared memory unavailable");
                }
                return false;
            }

            if (m_frameDeliveryMode == FrameDeliveryMode::PreviewLatest)
            {
                m_deliveryTimer.restart();
            }

            emitFrameAvailableEvent(frameIndex);
            return true;
        }
        catch (const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
            }
            return false;
        }
    }

    // Copy one camera frame into the shared memory ring buffer
    bool AgentRuntime::writeFrameToSharedMemory(const void* pixels,
                                                quint64 frameAdvance,
                                                quint64* frameIndexOut)
    {
        if (!m_shm || !m_shm->isAttached())
        {
            return false;
        }
        uchar* base = static_cast<uchar*>(m_shm->data());
        if (!base)
        {
            return false;
        }

        const quint64 nextFrameIndex = m_frameIndex + (std::max)(quint64{1}, frameAdvance);
        const int preferredSlotIndex = m_nextWriteSlot;
        int slotIndex = -1;
        uchar* ptr = nullptr;
        for (int offset = 0; offset < kSharedFrameNumSlots; ++offset)
        {
            const int candidate = (preferredSlotIndex + offset) % kSharedFrameNumSlots;
            uchar* candidatePtr = base + kSharedMemoryControlSize
                                  + candidate * kSharedFrameSlotStride;
            auto& stateValue = *reinterpret_cast<quint32*>(candidatePtr);
            std::atomic_ref<quint32> state(stateValue);
            quint32 expected = state.load(std::memory_order_acquire);
            while (expected == 0 || expected == 2)
            {
                if (state.compare_exchange_weak(expected,
                                                1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                {
                    slotIndex = candidate;
                    ptr = candidatePtr;
                    break;
                }
            }
            if (slotIndex >= 0)
            {
                break;
            }
        }

        if (slotIndex < 0 || !ptr)
        {
            m_frameIndex = nextFrameIndex;
            if (frameIndexOut)
            {
                *frameIndexOut = m_frameIndex;
            }
            return false;
        }

        auto* control = reinterpret_cast<SharedMemoryControl*>(base);
        m_nextWriteSlot = (slotIndex + 1) % kSharedFrameNumSlots;
        SharedFrameHeader header{};
        header.state = 1;
        header.width = m_frameLayout.width;
        header.height = m_frameLayout.height;
        header.stride = m_frameLayout.stride;
        header.pixelFormat = static_cast<quint32>(m_frameLayout.format);
        header.bitsPerSample = m_frameLayout.bitDepth;
        header.channels = m_frameLayout.channels;
        header.frameIndex = nextFrameIndex;
        header.timestampNs =
            static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        setSharedFrameSourceRoi(header,
                                m_sourceRoiX,
                                m_sourceRoiY,
                                m_sourceRoiWidth,
                                m_sourceRoiHeight);
        memcpy(ptr + sizeof(header.state),
               reinterpret_cast<const uchar*>(&header) + sizeof(header.state),
               sizeof(header) - sizeof(header.state));

        uchar* dst = ptr + kSharedFrameHeaderSize;
        memcpy(dst, pixels, static_cast<size_t>(m_frameLayout.byteCount));

        auto& stateValue = *reinterpret_cast<quint32*>(ptr);
        std::atomic_ref<quint32>(stateValue).store(2, std::memory_order_release);
        std::atomic_ref<quint32>(control->latestSlotIndex)
            .store(static_cast<quint32>(slotIndex), std::memory_order_release);
        m_frameIndex = nextFrameIndex;
        if (frameIndexOut)
        {
            *frameIndexOut = m_frameIndex;
        }

        return true;
    }

    // Drain camera frames into shared memory and publish the newest index
    void AgentRuntime::pollAndWrite()
    {
        if (!m_mmcore)
        {
            return;
        }

        try
        {
            long remaining = m_mmcore->getRemainingImageCount();

            if (remaining <= 0)
            {
                return;
            }

            quint64 newestFrameIndex = 0;
            quint64 acquiredFrameCount = 0;
            if (m_frameDeliveryMode != FrameDeliveryMode::AllFrames)
            {
                const bool previewRateLimited =
                    m_frameDeliveryMode == FrameDeliveryMode::PreviewLatest;
                const bool publishFrame = !previewRateLimited
                    || !m_deliveryTimer.isValid()
                    || m_deliveryTimer.elapsed() >= kPreviewFrameDeliveryIntervalMs;
                while (remaining-- > 0)
                {
                    const void* pixels = m_mmcore->popNextImage();
                    if (!pixels)
                    {
                        break;
                    }
                    ++acquiredFrameCount;
                    ++m_pendingFrameAdvance;
                    if (remaining > 0)
                    {
                        continue;
                    }
                    if (publishFrame && m_frameLayoutValid)
                    {
                        quint64 writtenFrameIndex = 0;
                        const bool written = writeFrameToSharedMemory(
                            pixels,
                            m_pendingFrameAdvance,
                            &writtenFrameIndex);
                        m_pendingFrameAdvance = 0;
                        if (previewRateLimited)
                        {
                            m_deliveryTimer.restart();
                        }
                        if (written)
                        {
                            newestFrameIndex = writtenFrameIndex;
                        }
                    }
                }
            }
            else
            {
                while (remaining-- > 0)
                {
                    const void* pixels = m_mmcore->popNextImage();
                    if (!pixels)
                    {
                        break;
                    }
                    ++acquiredFrameCount;

                    if (!m_frameLayoutValid)
                    {
                        break;
                    }

                    quint64 writtenFrameIndex = 0;
                    if (!writeFrameToSharedMemory(pixels, 1, &writtenFrameIndex))
                    {
                        break;
                    }
                    newestFrameIndex = writtenFrameIndex;
                }
            }

            if (newestFrameIndex != 0)
            {
                emitFrameAvailableEvent(newestFrameIndex);
            }
            updatePollingInterval(acquiredFrameCount);
        }
        catch (const CMMError& error)
        {
            const QString message =
                QStringLiteral("Agent capture error: %1")
                .arg(QString::fromStdString(error.getMsg()));
            setState(State::Error, message);
        }
    }

    // Adapts MMCore polling to the observed camera frame interval
    void AgentRuntime::updatePollingInterval(quint64 frameCount)
    {
        if (frameCount == 0 || !m_frameIntervalTimer.isValid())
        {
            return;
        }

        const double elapsedMs = static_cast<double>(m_frameIntervalTimer.nsecsElapsed()) / 1000000.0;
        m_frameIntervalTimer.restart();
        const double measuredIntervalMs = elapsedMs / static_cast<double>(frameCount);
        if (!std::isfinite(measuredIntervalMs) || measuredIntervalMs <= 0.0)
        {
            return;
        }

        m_observedFrameIntervalMs = m_observedFrameIntervalMs > 0.0
                                        ? 0.75 * m_observedFrameIntervalMs
                                              + 0.25 * measuredIntervalMs
                                        : measuredIntervalMs;
        const int intervalMs = pollingIntervalFor(m_observedFrameIntervalMs);
        if (m_timer->interval() != intervalMs)
        {
            m_timer->setInterval(intervalMs);
        }
    }

    // Validate and cache the current frame memory layout
    bool AgentRuntime::ensureFrameLayout(unsigned width, unsigned height, unsigned bytesPerPixel)
    {
        const auto logOversized = [this](quint64 byteCount)
        {
            if (!m_loggedOversizedFrame)
            {
                qWarning().noquote()
                    << QString("[Agent %1] Frame payload (%2 bytes) exceeds shared memory slot capacity (%3 bytes)")
                       .arg(m_cameraId)
                       .arg(static_cast<qulonglong>(byteCount))
                       .arg(kSharedFrameMaxBytes);
                m_loggedOversizedFrame = true;
            }
        };

        if (bytesPerPixel != 1 && bytesPerPixel != 2)
        {
            if (!m_loggedUnsupportedFormat)
            {
                qWarning().noquote()
                    << QString("[Agent %1] Unsupported bytes-per-pixel (%2). Only Mono8/Mono16 are supported.")
                       .arg(m_cameraId)
                       .arg(bytesPerPixel);
                m_loggedUnsupportedFormat = true;
            }
            return false;
        }
        m_loggedUnsupportedFormat = false;

        const bool geometryChanged =
            !m_frameLayoutValid
            || m_frameLayout.width != width
            || m_frameLayout.height != height
            || m_frameLayout.bytesPerPixel != bytesPerPixel;

        if (!geometryChanged)
        {
            if (m_frameLayout.byteCount == 0
                || m_frameLayout.byteCount > static_cast<quint64>(kSharedFrameMaxBytes))
            {
                logOversized(m_frameLayout.byteCount);
                return false;
            }
            if (m_sourceRoiWidth <= 0 || m_sourceRoiHeight <= 0)
            {
                refreshSourceRoi();
            }
            m_loggedOversizedFrame = false;
            return true;
        }

        m_frameLayoutValid = false;

        FrameLayout updated;
        updated.width = width;
        updated.height = height;
        updated.bytesPerPixel = bytesPerPixel;
        updated.format =
            (bytesPerPixel == 2) ? SharedPixelFormat::Mono16 : SharedPixelFormat::Mono8;
        updated.channels = 1;
        const quint64 stride = static_cast<quint64>(width) * bytesPerPixel;
        if (stride == 0 || stride > (std::numeric_limits<unsigned>::max)())
        {
            logOversized(stride);
            return false;
        }
        updated.stride = static_cast<unsigned>(stride);
        updated.byteCount = computeMaxFrameBytes(width, height, updated.format);

        if (updated.byteCount == 0
            || updated.byteCount > static_cast<quint64>(kSharedFrameMaxBytes))
        {
            logOversized(updated.byteCount);
            return false;
        }

        quint16 bitDepth =
            (updated.format == SharedPixelFormat::Mono16) ? 16 : 8;
        try
        {
            bitDepth = normalizedSharedBitDepth(
                updated.format,
                static_cast<int>(m_mmcore->getImageBitDepth()));
        }
        catch (const CMMError&)
        {
            bitDepth =
                (updated.format == SharedPixelFormat::Mono16) ? 16 : 8;
        }
        updated.bitDepth = normalizedSharedBitDepth(updated.format, bitDepth);

        m_frameLayout = updated;
        m_frameLayoutValid = true;
        refreshSourceRoi();
        m_loggedOversizedFrame = false;
        return true;
    }

    // Read the active camera ROI used by following frame metadata
    void AgentRuntime::refreshSourceRoi()
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        try
        {
            m_mmcore->getROI(m_cameraId.toStdString().c_str(), x, y, width, height);
        }
        catch (const CMMError&)
        {
            width = static_cast<int>(m_frameLayout.width);
            height = static_cast<int>(m_frameLayout.height);
        }

        if (width <= 0 || height <= 0)
        {
            width = static_cast<int>(m_frameLayout.width);
            height = static_cast<int>(m_frameLayout.height);
        }

        m_sourceRoiX = x;
        m_sourceRoiY = y;
        m_sourceRoiWidth = width;
        m_sourceRoiHeight = height;
    }

    class Agent final : public QObject
    {
        Q_OBJECT

    public:
        // Create the control server wrapper for one camera agent
        Agent(QString cameraId,
              QString adapter,
              QString device,
              QString shmKey,
              QStringList preInitProperties,
              QStringList properties,
              double exposureMs,
              bool autoPreview,
              QObject* parent = nullptr)
            : QObject(parent)
              , m_cameraId(std::move(cameraId))
              , m_adapter(std::move(adapter))
              , m_device(std::move(device))
              , m_shmKey(std::move(shmKey))
              , m_serverName(agent::controlServerName(m_cameraId))
              , m_preInitProperties(std::move(preInitProperties))
              , m_properties(std::move(properties))
              , m_exposureMs(exposureMs)
              , m_autoPreview(autoPreview)
        {
            qRegisterMetaType<QJsonObject>("QJsonObject");
            qRegisterMetaType<quint64>("quint64");
        }

        // Stop the runtime thread before destruction
        ~Agent() override
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

        QString m_cameraId;
        QString m_adapter;
        QString m_device;
        QString m_shmKey;
        QString m_serverName;
        QStringList m_preInitProperties;
        QStringList m_properties;
        double m_exposureMs{0.0};
        bool m_autoPreview{false};

        std::unique_ptr<QLocalServer> m_ctrlServer;
        QThread m_runtimeThread;
        AgentRuntime* m_runtime{nullptr};
        quint64 m_nextConnectionId{1};
        QHash<quint64, ControlConnection*> m_connections;
    };

    // Start the runtime thread and local control server
    bool Agent::start()
    {
        m_runtime = new AgentRuntime(m_cameraId,
                                     m_adapter,
                                     m_device,
                                     m_shmKey,
                                     m_preInitProperties,
                                     m_properties,
                                     m_exposureMs,
                                     m_autoPreview);
        m_runtime->moveToThread(&m_runtimeThread);
        connect(&m_runtimeThread, &QThread::finished,
                m_runtime, &QObject::deleteLater);
        connect(m_runtime, &AgentRuntime::responseReady,
                this, &Agent::onRuntimeResponse);
        connect(m_runtime, &AgentRuntime::eventReady,
                this, &Agent::broadcastEvent);
        connect(m_runtime, &AgentRuntime::shutdownRequested, this,
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
                << QString("Agent init failed for '%1': %2")
                .arg(m_cameraId, initError);
            stopRuntime();
            return false;
        }

        QLocalServer::removeServer(m_serverName);
        m_ctrlServer = std::make_unique<QLocalServer>(this);
        connect(m_ctrlServer.get(), &QLocalServer::newConnection,
                this, &Agent::onNewControlConnection);
        if (!m_ctrlServer->listen(m_serverName))
        {
            qCritical().noquote()
                << QString("Agent control server failed to listen on %1")
                .arg(m_serverName);
            stopRuntime();
            return false;
        }

        qInfo().noquote()
            << QString("Agent control server listening on %1").arg(m_serverName);
        return true;
    }

    // Accept pending local control connections
    void Agent::onNewControlConnection()
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
                    m_runtime, &AgentRuntime::handleRequest,
                    Qt::QueuedConnection);
            connect(connection, &ControlConnection::connectionClosed,
                    this, &Agent::onConnectionClosed);

            QMetaObject::invokeMethod(m_runtime,
                                      &AgentRuntime::publishHello,
                                      Qt::QueuedConnection);
        }
    }

    // Route one runtime response back to its connection
    void Agent::onRuntimeResponse(quint64 connectionId, const QJsonObject& response)
    {
        auto it = m_connections.find(connectionId);
        if (it == m_connections.end() || !it.value())
        {
            return;
        }
        it.value()->sendMessage(response);
    }

    // Broadcast one runtime event to all clients
    void Agent::broadcastEvent(const QJsonObject& event)
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
    void Agent::onConnectionClosed(quint64 connectionId)
    {
        m_connections.remove(connectionId);
    }

    // Stop the runtime worker thread cleanly
    void Agent::stopRuntime()
    {
        if (!m_runtime)
        {
            return;
        }

        QMetaObject::invokeMethod(m_runtime,
                                  &AgentRuntime::stopForExit,
                                  Qt::BlockingQueuedConnection);
        m_runtimeThread.quit();
        m_runtimeThread.wait();
        m_runtime = nullptr;
    }
} // namespace scopeone::core::internal

// Launch one camera agent process from command line arguments
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("ScopeOne Camera Agent");
    parser.addHelpOption();

    QCommandLineOption optCamId({QStringLiteral("c"), QStringLiteral("cameraId")},
                                QStringLiteral("Camera ID"),
                                QStringLiteral("id"));
    QCommandLineOption optAdapter(QStringLiteral("adapter"),
                                  QStringLiteral("MM adapter"),
                                  QStringLiteral("adapter"));
    QCommandLineOption optDevice(QStringLiteral("device"),
                                 QStringLiteral("MM device"),
                                 QStringLiteral("device"));
    QCommandLineOption optShm(QStringLiteral("shm"),
                              QStringLiteral("Shared memory key"),
                              QStringLiteral("key"));
    QCommandLineOption optExp(QStringLiteral("exposure"),
                              QStringLiteral("Exposure ms"),
                              QStringLiteral("ms"));
    QCommandLineOption optPreInit(QStringLiteral("preinit"),
                                  QStringLiteral("JSON-encoded pre-init property"),
                                  QStringLiteral("json"));
    QCommandLineOption optProperty(QStringLiteral("property"),
                                   QStringLiteral("JSON-encoded initialized property"),
                                   QStringLiteral("json"));
    QCommandLineOption optAuto(QStringLiteral("autoPreview"),
                               QStringLiteral("Start preview immediately"));

    parser.addOption(optCamId);
    parser.addOption(optAdapter);
    parser.addOption(optDevice);
    parser.addOption(optShm);
    parser.addOption(optExp);
    parser.addOption(optPreInit);
    parser.addOption(optProperty);
    parser.addOption(optAuto);
    parser.process(app);

    if (!parser.isSet(optCamId)
        || !parser.isSet(optAdapter)
        || !parser.isSet(optDevice)
        || !parser.isSet(optShm))
    {
        qCritical().noquote()
            << "Missing required arguments: --cameraId, --adapter, --device, --shm";
        return 2;
    }

    scopeone::core::internal::Agent agent(parser.value(optCamId),
                                          parser.value(optAdapter),
                                          parser.value(optDevice),
                                          parser.value(optShm),
                                          parser.values(optPreInit),
                                          parser.values(optProperty),
                                          parser.isSet(optExp)
                                              ? parser.value(optExp).toDouble()
                                              : 0.0,
                                          parser.isSet(optAuto));
    if (!agent.start())
    {
        return 2;
    }

    return app.exec();
}

#include "AgentMain.moc"
