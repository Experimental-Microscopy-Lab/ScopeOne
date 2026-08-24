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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "MMCore.h"
#include "internal/DriverHostProtocol.h"
#include "scopeone/CameraProvider.h"
#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/PluginManifest.h"
#include "scopeone/HardwareCapabilities.h"
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

    class DriverHostRuntime : public QObject
    {
        Q_OBJECT

    public:
        using QObject::QObject;
        virtual bool initializeRuntime() = 0;
        virtual QString lastError() const = 0;
        virtual void publishHello() = 0;
        virtual void handleRequest(quint64 connectionId,
                                   quint64 requestId,
                                   const QString& type,
                                   const QJsonObject& message) = 0;
        virtual void stopForExit() = 0;

    signals:
        void responseReady(quint64 connectionId, const QJsonObject& response);
        void eventReady(const QJsonObject& event);
        void shutdownRequested();
    };

    class MicroManagerDriverRuntime final : public DriverHostRuntime
    {
        Q_OBJECT

    public:
        // Store launch settings for one camera runtime
        MicroManagerDriverRuntime(QString providerId,
                          QString cameraId,
                          QString adapter,
                          QString device,
                          QString shmKey,
                          QStringList preInitProperties,
                          QStringList properties,
                          double exposureMs,
                          bool autoPreview,
                          QObject* parent = nullptr)
            : DriverHostRuntime(parent)
              , m_providerId(std::move(providerId))
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

        QString lastError() const override
        {
            return m_lastError;
        }

        // Initialize MMCore camera state and shared memory transport
        bool initializeRuntime() override
        {
            m_lastError.clear();
            setState(State::Starting);

            if (!m_timer)
            {
                m_timer = new QTimer(this);
                m_timer->setTimerType(Qt::PreciseTimer);
                connect(m_timer, &QTimer::timeout,
                        this, &MicroManagerDriverRuntime::pollAndWrite);
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
                }
                m_mmcore->waitForDevice(label.c_str());
                finalExposure = m_mmcore->getExposure();
                if (finalExposure <= 0.0)
                {
                    m_lastError = QStringLiteral("Camera reported an invalid exposure");
                    setState(State::Error, m_lastError);
                    return false;
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

        void publishHello() override;
        void handleRequest(quint64 connectionId,
                           quint64 requestId,
                           const QString& type,
                           const QJsonObject& message) override;
        void stopForExit() override;

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
        void emitDriverHostErrorEvent(const QString& error);
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

        QString m_providerId;
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
    void MicroManagerDriverRuntime::publishHello()
    {
        QJsonObject event = makeEvent(driverhost::kEventHello);
        event.insert(driverhost::kProviderIdField, m_providerId);
        event.insert(driverhost::kDeviceIdField, m_cameraId);
        event.insert(driverhost::kDeviceKindField, driverhost::kCapabilityCamera);
        event.insert(driverhost::kCapabilitiesField,
                     QJsonArray{driverhost::kCapabilityCamera,
                                driverhost::kCapabilityProperties});
        emit eventReady(event);
    }

    // Replay encoded cfg properties into the DriverHost MMCore instance
    bool MicroManagerDriverRuntime::applyProperties(const QStringList& encodedProperties,
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
    void MicroManagerDriverRuntime::handleRequest(quint64 connectionId,
                                     quint64 requestId,
                                     const QString& type,
                                     const QJsonObject& message)
    {
        if (requestId == 0 || type.isEmpty())
        {
            return;
        }

        if (!m_mmcore && type != driverhost::kCommandShutdown)
        {
            emit responseReady(connectionId,
                               makeErrorResponse(type,
                                                 requestId,
                                                 QStringLiteral("DriverHost runtime not initialized")));
            return;
        }

        if (type == driverhost::kCommandDescribe)
        {
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(driverhost::kProviderIdField, m_providerId);
            response.insert(driverhost::kDeviceIdField, m_cameraId);
            response.insert(driverhost::kDeviceKindField, driverhost::kCapabilityCamera);
            response.insert(driverhost::kCapabilitiesField,
                            QJsonArray{driverhost::kCapabilityCamera,
                                       driverhost::kCapabilityProperties});
            emit responseReady(connectionId, response);
            return;
        }

        if (type == driverhost::kCommandStartPreview)
        {
            QString error;
            const bool ok = startPreviewInternal(&error);
            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandStopPreview)
        {
            QString error;
            const bool ok = stopPreviewInternal(&error);
            emit responseReady(connectionId,
                               ok
                                   ? makeResponse(type, requestId, true)
                                   : makeErrorResponse(type, requestId, error));
            return;
        }

        if (type == driverhost::kCommandSetFrameDeliveryMode)
        {
            const QString mode = message.value(QStringLiteral("mode")).toString();
            if (mode == driverhost::kFrameDeliveryModePreviewLatest)
            {
                m_frameDeliveryMode = FrameDeliveryMode::PreviewLatest;
                m_deliveryTimer.invalidate();
            }
            else if (mode == driverhost::kFrameDeliveryModeLatestOnly)
            {
                m_frameDeliveryMode = FrameDeliveryMode::LatestOnly;
            }
            else if (mode == driverhost::kFrameDeliveryModeAllFrames)
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

        if (type == driverhost::kCommandGetExposure)
        {
            try
            {
                m_exposureMs = m_mmcore->getExposure();
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("exposureMs"), m_exposureMs);
                emit responseReady(connectionId, response);
            }
            catch (const CMMError& mmError)
            {
                emit responseReady(
                    connectionId,
                    makeErrorResponse(type,
                                      requestId,
                                      QString::fromStdString(mmError.getMsg())));
            }
            return;
        }

        if (type == driverhost::kCommandSetExposure)
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

        if (type == driverhost::kCommandListProperties)
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

        if (type == driverhost::kCommandGetProperty)
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

        if (type == driverhost::kCommandSetProperty)
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
                m_mmcore->waitForDevice(m_cameraId.toStdString().c_str());
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

        if (type == driverhost::kCommandSetRoi)
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

        if (type == driverhost::kCommandClearRoi)
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

        if (type == driverhost::kCommandGetRoi)
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

        if (type == driverhost::kCommandCaptureEvent)
        {
            QString error;
            quint64 frameIndex = 0;
            const bool ok = captureEventFrameInternal(frameIndex, &error);
            if (ok)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
                emit responseReady(connectionId, response);
            }
            else
            {
                emit responseReady(connectionId, makeErrorResponse(type, requestId, error));
            }
            return;
        }

        if (type == driverhost::kCommandShutdown)
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
    void MicroManagerDriverRuntime::stopForExit()
    {
        stopPreviewInternal(nullptr);
        setState(State::ShuttingDown);
    }

    // Check whether the runtime is currently previewing
    bool MicroManagerDriverRuntime::previewRunning() const
    {
        return m_state == State::Previewing;
    }

    // Update runtime state and publish state changes
    void MicroManagerDriverRuntime::setState(State state, const QString& error)
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
            emitDriverHostErrorEvent(error);
        }
    }

    // Build a protocol response envelope
    QJsonObject MicroManagerDriverRuntime::makeResponse(const QString& type, quint64 requestId, bool ok) const
    {
        QJsonObject response =
            driverhost::makeEnvelope(driverhost::kMessageKindResponse, type, requestId);
        response.insert(QStringLiteral("ok"), ok);
        return response;
    }

    // Build a protocol error response envelope
    QJsonObject MicroManagerDriverRuntime::makeErrorResponse(const QString& type,
                                                quint64 requestId,
                                                const QString& error) const
    {
        QJsonObject response = makeResponse(type, requestId, false);
        response.insert(QStringLiteral("error"), error);
        return response;
    }

    // Build a protocol event envelope
    QJsonObject MicroManagerDriverRuntime::makeEvent(const QString& type) const
    {
        return driverhost::makeEnvelope(driverhost::kMessageKindEvent, type);
    }

    // Publish current preview state to clients
    void MicroManagerDriverRuntime::emitPreviewStateEvent()
    {
        QJsonObject event = makeEvent(driverhost::kEventPreviewState);
        event.insert(QStringLiteral("running"), previewRunning());
        emit eventReady(event);
    }

    // Publish a DriverHost error event to clients
    void MicroManagerDriverRuntime::emitDriverHostErrorEvent(const QString& error)
    {
        QJsonObject event = makeEvent(driverhost::kEventDriverHostError);
        event.insert(QStringLiteral("error"), error);
        emit eventReady(event);
    }

    // Publish the newest shared memory frame index
    void MicroManagerDriverRuntime::emitFrameAvailableEvent(quint64 frameIndex)
    {
        QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
        event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
        emit eventReady(event);
    }

    // Start continuous acquisition and polling
    bool MicroManagerDriverRuntime::startPreviewInternal(QString* errorMessage)
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
                *errorMessage = QStringLiteral("DriverHost is not in a runnable state");
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
    bool MicroManagerDriverRuntime::stopPreviewInternal(QString* errorMessage)
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
    bool MicroManagerDriverRuntime::captureEventFrameInternal(quint64& frameIndex, QString* errorMessage)
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
    bool MicroManagerDriverRuntime::writeFrameToSharedMemory(const void* pixels,
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
    void MicroManagerDriverRuntime::pollAndWrite()
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
                QStringLiteral("DriverHost capture error: %1")
                .arg(QString::fromStdString(error.getMsg()));
            setState(State::Error, message);
        }
    }

    // Adapts MMCore polling to the observed camera frame interval
    void MicroManagerDriverRuntime::updatePollingInterval(quint64 frameCount)
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
    bool MicroManagerDriverRuntime::ensureFrameLayout(unsigned width, unsigned height, unsigned bytesPerPixel)
    {
        const auto logOversized = [this](quint64 byteCount)
        {
            if (!m_loggedOversizedFrame)
            {
                qWarning().noquote()
                    << QString("[DriverHost %1] Frame payload (%2 bytes) exceeds shared memory slot capacity (%3 bytes)")
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
                    << QString("[DriverHost %1] Unsupported bytes-per-pixel (%2). Only Mono8/Mono16 are supported.")
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
    void MicroManagerDriverRuntime::refreshSourceRoi()
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

    class ProviderDriverRuntime final : public DriverHostRuntime
    {
    public:
        ProviderDriverRuntime(QString pluginPath,
                              QString providerId,
                              QString hostKey,
                              QJsonObject options,
                              QObject* parent = nullptr)
            : DriverHostRuntime(parent)
              , m_pluginPath(std::move(pluginPath))
              , m_providerId(std::move(providerId))
              , m_hostKey(std::move(hostKey))
              , m_options(std::move(options))
        {
        }

        ~ProviderDriverRuntime() override
        {
            stopForExit();
        }

        bool initializeRuntime() override;
        QString lastError() const override { return m_lastError; }
        void publishHello() override;
        void handleRequest(quint64 connectionId,
                           quint64 requestId,
                           const QString& type,
                           const QJsonObject& message) override;
        void stopForExit() override;

    private:
        enum class FrameDeliveryMode
        {
            PreviewLatest,
            LatestOnly,
            AllFrames
        };

        struct CameraTransport
        {
            QString deviceId;
            QString shmKey;
            std::unique_ptr<QSharedMemory> shm;
            std::atomic<FrameDeliveryMode> deliveryMode{FrameDeliveryMode::PreviewLatest};
            QMutex frameMutex;
            QMutex writeMutex;
            ImageFrame latestFrame;
            bool latestDispatchQueued{false};
            ImageFrame previewFrame;
            QTimer* previewTimer{nullptr};
            QElapsedTimer deliveryTimer;
            quint64 frameIndex{0};
            int nextWriteSlot{0};
            bool previewRunning{false};
        };

        QJsonObject makeResponse(const QString& type, quint64 requestId, bool ok) const;
        QJsonObject makeErrorResponse(const QString& type,
                                      quint64 requestId,
                                      const QString& error) const;
        QJsonObject makeEvent(const QString& type) const;
        QJsonArray capabilities(const HardwareDeviceDescriptor& device) const;
        bool supportsCapability(const HardwareDeviceDescriptor& device,
                                const QString& capability) const;
        QString deviceKindName(HardwareDeviceKind kind) const;
        QJsonObject deviceDescription(const HardwareDeviceDescriptor& device) const;
        const HardwareDeviceDescriptor* requestedDevice(const QJsonObject& message) const;
        std::shared_ptr<CameraTransport> cameraTransport(const QString& deviceId) const;
        bool createSharedMemory(CameraTransport& transport);
        void enqueueFrame(const ImageFrame& frame);
        void dispatchLatestFrame(const QString& deviceId);
        void publishAllFrame(const QString& deviceId, const ImageFrame& frame);
        void publishFrame(const QString& deviceId, const ImageFrame& frame);
        void flushPreviewFrame(const QString& deviceId);
        bool writeFrame(CameraTransport& transport,
                        const ImageFrame& frame,
                        quint64* frameIndexOut = nullptr);
        void emitPreviewStateEvent(const QString& deviceId, bool running);

        QString m_pluginPath;
        QString m_providerId;
        QString m_hostKey;
        QJsonObject m_options;
        QString m_lastError;
        HardwareProviderDescriptor m_descriptor;
        QList<HardwareDeviceDescriptor> m_devices;
        QHash<QString, HardwareDeviceDescriptor> m_devicesById;
        QHash<QString, std::shared_ptr<CameraTransport>> m_cameraTransports;
        mutable QReadWriteLock m_cameraTransportsLock;
        std::unique_ptr<QPluginLoader> m_pluginLoader;
        HardwareProviderPtr m_provider;
        CameraProvider* m_camera{nullptr};
        DevicePropertyProvider* m_properties{nullptr};
        StageProvider* m_stage{nullptr};
        ShutterProvider* m_shutter{nullptr};
        StateProvider* m_state{nullptr};
        ConfigurationProvider* m_configuration{nullptr};
        std::atomic_bool m_acceptFrames{false};
    };

    bool ProviderDriverRuntime::initializeRuntime()
    {
        m_lastError.clear();
        m_pluginLoader = std::make_unique<QPluginLoader>(m_pluginPath);
        PluginManifest manifest;
        if (!parsePluginManifest(
                m_pluginLoader->metaData().value(QStringLiteral("MetaData")).toObject(),
                PluginKind::Hardware,
                manifest,
                &m_lastError))
        {
            return false;
        }
        if (manifest.id != m_providerId
            || manifest.metadata.value(QStringLiteral("providerId")).toString().trimmed()
                   != m_providerId)
        {
            m_lastError = QStringLiteral("Provider manifest identity mismatch");
            return false;
        }
        QObject* const instance = m_pluginLoader->instance();
        if (!instance)
        {
            m_lastError = m_pluginLoader->errorString();
            return false;
        }

        auto* const factory = qobject_cast<DriverHostProviderPlugin*>(instance);
        if (!factory)
        {
            m_lastError = QStringLiteral("Module does not implement DriverHostProviderPlugin");
            return false;
        }
        if (factory->providerId() != m_providerId)
        {
            m_lastError = QStringLiteral("Provider module identity mismatch");
            return false;
        }

        m_options.insert(QStringLiteral("providerId"), m_providerId);
        m_provider = factory->createProvider(m_options, &m_lastError);
        if (!m_provider)
        {
            if (m_lastError.isEmpty())
            {
                m_lastError = QStringLiteral("Provider module returned no provider");
            }
            return false;
        }
        m_descriptor = m_provider->descriptor();
        if (m_descriptor.id != m_providerId
            || m_descriptor.id != m_descriptor.id.trimmed())
        {
            m_lastError = QStringLiteral("Provider descriptor identity mismatch");
            return false;
        }

        HardwareProvider* const provider = m_provider.get();
        m_camera = dynamic_cast<CameraProvider*>(provider);
        m_properties = dynamic_cast<DevicePropertyProvider*>(provider);
        m_stage = dynamic_cast<StageProvider*>(provider);
        m_shutter = dynamic_cast<ShutterProvider*>(provider);
        m_state = dynamic_cast<StateProvider*>(provider);
        m_configuration = dynamic_cast<ConfigurationProvider*>(provider);

        QSet<QString> logicalIds;
        int cameraIndex = 0;
        for (HardwareDeviceDescriptor device : m_provider->devices())
        {
            const QString logicalId = device.logicalId.trimmed();
            const bool capabilityAvailable =
                (device.kind != HardwareDeviceKind::Camera || m_camera)
                && ((device.kind != HardwareDeviceKind::XYStage
                     && device.kind != HardwareDeviceKind::ZStage)
                    || m_stage)
                && (device.kind != HardwareDeviceKind::Shutter || m_shutter)
                && (device.kind != HardwareDeviceKind::State || m_state);
            if (logicalId.isEmpty()
                || device.logicalId != logicalId
                || device.providerId != m_providerId
                || logicalIds.contains(logicalId)
                || !capabilityAvailable)
            {
                m_lastError = QStringLiteral("Provider returned an invalid device catalog");
                return false;
            }
            logicalIds.insert(logicalId);
            device.endpoint = HardwareEndpointKind::DriverHost;
            m_devices.append(device);
            m_devicesById.insert(logicalId, device);

            if (device.kind == HardwareDeviceKind::Camera)
            {
                auto transport = std::make_shared<CameraTransport>();
                transport->deviceId = logicalId;
                transport->shmKey = driverhost::sharedMemoryKey(m_hostKey, cameraIndex++);
                transport->previewTimer = new QTimer(this);
                transport->previewTimer->setSingleShot(true);
                transport->previewTimer->setTimerType(Qt::PreciseTimer);
                connect(transport->previewTimer, &QTimer::timeout, this,
                        [this, logicalId]() { flushPreviewFrame(logicalId); });
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
            m_camera->setFrameSink([this](const ImageFrame& frame) { enqueueFrame(frame); });
        }
        return true;
    }

    QJsonObject ProviderDriverRuntime::makeResponse(const QString& type,
                                                    quint64 requestId,
                                                    bool ok) const
    {
        QJsonObject response = driverhost::makeEnvelope(driverhost::kMessageKindResponse,
                                                        type,
                                                        requestId);
        response.insert(QStringLiteral("ok"), ok);
        return response;
    }

    QJsonObject ProviderDriverRuntime::makeErrorResponse(const QString& type,
                                                         quint64 requestId,
                                                         const QString& error) const
    {
        QJsonObject response = makeResponse(type, requestId, false);
        response.insert(QStringLiteral("error"), error);
        return response;
    }

    QJsonObject ProviderDriverRuntime::makeEvent(const QString& type) const
    {
        QJsonObject event = driverhost::makeEnvelope(driverhost::kMessageKindEvent, type);
        event.insert(driverhost::kProviderIdField, m_providerId);
        return event;
    }

    QJsonArray ProviderDriverRuntime::capabilities(
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
        if (m_configuration) result.append(driverhost::kCapabilityConfiguration);
        return result;
    }

    bool ProviderDriverRuntime::supportsCapability(
        const HardwareDeviceDescriptor& device,
        const QString& capability) const
    {
        return capabilities(device).contains(QJsonValue(capability));
    }

    QString ProviderDriverRuntime::deviceKindName(HardwareDeviceKind kind) const
    {
        switch (kind)
        {
        case HardwareDeviceKind::Camera: return QStringLiteral("Camera");
        case HardwareDeviceKind::XYStage: return QStringLiteral("XYStage");
        case HardwareDeviceKind::ZStage: return QStringLiteral("ZStage");
        case HardwareDeviceKind::Shutter: return QStringLiteral("Shutter");
        case HardwareDeviceKind::State: return QStringLiteral("State");
        case HardwareDeviceKind::Hub: return QStringLiteral("Hub");
        case HardwareDeviceKind::Serial: return QStringLiteral("Serial");
        case HardwareDeviceKind::Generic: return QStringLiteral("Generic");
        case HardwareDeviceKind::AutoFocus: return QStringLiteral("AutoFocus");
        case HardwareDeviceKind::ImageProcessor: return QStringLiteral("ImageProcessor");
        case HardwareDeviceKind::SignalIO: return QStringLiteral("SignalIO");
        case HardwareDeviceKind::Magnifier: return QStringLiteral("Magnifier");
        case HardwareDeviceKind::SLM: return QStringLiteral("SLM");
        case HardwareDeviceKind::Galvo: return QStringLiteral("Galvo");
        case HardwareDeviceKind::PressurePump: return QStringLiteral("PressurePump");
        case HardwareDeviceKind::VolumetricPump: return QStringLiteral("VolumetricPump");
        case HardwareDeviceKind::Unknown: break;
        }
        return QStringLiteral("Unknown");
    }

    QJsonObject ProviderDriverRuntime::deviceDescription(
        const HardwareDeviceDescriptor& device) const
    {
        QJsonObject object;
        object.insert(driverhost::kDeviceIdField, device.logicalId);
        object.insert(driverhost::kProviderDeviceIdField, device.providerDeviceId);
        object.insert(driverhost::kHardwareIdField, device.hardwareId);
        object.insert(driverhost::kDeviceNameField, device.name);
        object.insert(driverhost::kDeviceKindField, deviceKindName(device.kind));
        object.insert(driverhost::kDeviceStateField, static_cast<int>(device.state));
        object.insert(driverhost::kDevicePropertiesField,
                      QJsonObject::fromVariantMap(device.properties));
        object.insert(driverhost::kCapabilitiesField, capabilities(device));
        if (const auto transport = cameraTransport(device.logicalId))
        {
            object.insert(driverhost::kSharedMemoryKeyField, transport->shmKey);
        }
        return object;
    }

    const HardwareDeviceDescriptor* ProviderDriverRuntime::requestedDevice(
        const QJsonObject& message) const
    {
        const QString deviceId = message.value(driverhost::kDeviceIdField)
                                     .toString().trimmed();
        const auto it = m_devicesById.constFind(deviceId);
        return it == m_devicesById.cend() ? nullptr : &it.value();
    }

    std::shared_ptr<ProviderDriverRuntime::CameraTransport>
    ProviderDriverRuntime::cameraTransport(const QString& deviceId) const
    {
        QReadLocker locker(&m_cameraTransportsLock);
        return m_cameraTransports.value(deviceId.trimmed());
    }

    void ProviderDriverRuntime::publishHello()
    {
        QJsonObject event = makeEvent(driverhost::kEventHello);
        event.insert(driverhost::kProviderIdField, m_providerId);
        emit eventReady(event);
    }

    bool ProviderDriverRuntime::createSharedMemory(CameraTransport& transport)
    {
        transport.shm = std::make_unique<QSharedMemory>();
        transport.shm->setNativeKey(transport.shmKey);
        const int totalBytes =
            kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
        if (!transport.shm->create(totalBytes))
        {
            if (transport.shm->attach())
            {
                transport.shm->detach();
            }
            if (!transport.shm->create(totalBytes))
            {
                m_lastError = QStringLiteral("Cannot create shared memory '%1'")
                                  .arg(transport.shmKey);
                return false;
            }
        }
        if (!transport.shm->lock())
        {
            m_lastError = QStringLiteral("Cannot initialize shared memory '%1'")
                              .arg(transport.shmKey);
            return false;
        }
        auto* const base = static_cast<uchar*>(transport.shm->data());
        const SharedMemoryControl control{};
        memcpy(base, &control, sizeof(control));
        for (int index = 0; index < kSharedFrameNumSlots; ++index)
        {
            const SharedFrameHeader header{};
            memcpy(base + kSharedMemoryControlSize + index * kSharedFrameSlotStride,
                   &header,
                   sizeof(header));
        }
        transport.shm->unlock();
        return true;
    }

    void ProviderDriverRuntime::enqueueFrame(const ImageFrame& frame)
    {
        if (!m_acceptFrames.load(std::memory_order_acquire))
        {
            return;
        }
        const QString deviceId = frame.cameraId.trimmed();
        const auto transport = cameraTransport(deviceId);
        if (!transport)
        {
            return;
        }
        if (transport->deliveryMode.load(std::memory_order_relaxed)
            == FrameDeliveryMode::AllFrames)
        {
            publishAllFrame(deviceId, frame);
            return;
        }

        {
            QMutexLocker locker(&transport->frameMutex);
            transport->latestFrame = frame;
            if (transport->latestDispatchQueued)
            {
                return;
            }
            transport->latestDispatchQueued = true;
        }
        QMetaObject::invokeMethod(this,
                                  [this, deviceId]() { dispatchLatestFrame(deviceId); },
                                  Qt::QueuedConnection);
    }

    void ProviderDriverRuntime::dispatchLatestFrame(const QString& deviceId)
    {
        const auto transport = cameraTransport(deviceId);
        if (!transport) return;
        ImageFrame frame;
        {
            QMutexLocker locker(&transport->frameMutex);
            frame = transport->latestFrame;
            transport->latestDispatchQueued = false;
        }
        publishFrame(deviceId, frame);
    }

    void ProviderDriverRuntime::publishAllFrame(const QString& deviceId,
                                                const ImageFrame& frame)
    {
        const auto transport = cameraTransport(deviceId);
        if (!transport) return;
        quint64 frameIndex = 0;
        if (!writeFrame(*transport, frame, &frameIndex)) return;

        QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
        event.insert(driverhost::kProviderIdField, m_providerId);
        event.insert(driverhost::kDeviceIdField, deviceId);
        event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
        emit eventReady(event);
    }

    void ProviderDriverRuntime::publishFrame(const QString& deviceId,
                                             const ImageFrame& frame)
    {
        const auto transport = cameraTransport(deviceId);
        if (!transport) return;
        if (transport->deliveryMode.load(std::memory_order_relaxed)
            == FrameDeliveryMode::PreviewLatest)
        {
            transport->previewFrame = frame;
            const int remaining = transport->deliveryTimer.isValid()
                                      ? kPreviewFrameDeliveryIntervalMs
                                            - static_cast<int>(transport->deliveryTimer.elapsed())
                                      : 0;
            if (remaining > 0)
            {
                if (!transport->previewTimer->isActive())
                {
                    transport->previewTimer->start(remaining);
                }
                return;
            }
        }

        quint64 frameIndex = 0;
        if (writeFrame(*transport, frame, &frameIndex))
        {
            if (transport->deliveryMode.load(std::memory_order_relaxed)
                == FrameDeliveryMode::PreviewLatest)
            {
                transport->deliveryTimer.restart();
            }
            QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
            event.insert(driverhost::kProviderIdField, m_providerId);
            event.insert(driverhost::kDeviceIdField, deviceId);
            event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
            emit eventReady(event);
        }
    }

    void ProviderDriverRuntime::flushPreviewFrame(const QString& deviceId)
    {
        const auto transport = cameraTransport(deviceId);
        if (!transport) return;
        const ImageFrame frame = transport->previewFrame;
        transport->previewFrame = {};
        quint64 frameIndex = 0;
        if (writeFrame(*transport, frame, &frameIndex))
        {
            transport->deliveryTimer.restart();
            QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
            event.insert(driverhost::kProviderIdField, m_providerId);
            event.insert(driverhost::kDeviceIdField, deviceId);
            event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
            emit eventReady(event);
        }
    }

    bool ProviderDriverRuntime::writeFrame(CameraTransport& transport,
                                           const ImageFrame& frame,
                                           quint64* frameIndexOut)
    {
        if (!transport.shm || !transport.shm->isAttached() || !frame.isValid()
            || frame.cameraId.trimmed() != transport.deviceId
            || frame.bytes.size() > kSharedFrameMaxBytes)
        {
            return false;
        }
        QMutexLocker writeLocker(&transport.writeMutex);
        auto* const base = static_cast<uchar*>(transport.shm->data());
        if (!base)
        {
            return false;
        }

        transport.frameIndex = (std::max)(transport.frameIndex + 1, frame.frameIndex);
        const quint64 frameIndex = transport.frameIndex;

        int slotIndex = -1;
        uchar* slot = nullptr;
        for (int offset = 0; offset < kSharedFrameNumSlots; ++offset)
        {
            const int candidate = (transport.nextWriteSlot + offset) % kSharedFrameNumSlots;
            uchar* const candidateSlot = base + kSharedMemoryControlSize
                                         + candidate * kSharedFrameSlotStride;
            auto& stateValue = *reinterpret_cast<quint32*>(candidateSlot);
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
                    slot = candidateSlot;
                    break;
                }
            }
            if (slot)
            {
                break;
            }
        }
        if (!slot)
        {
            return false;
        }

        transport.nextWriteSlot = (slotIndex + 1) % kSharedFrameNumSlots;
        SharedFrameHeader header = frame.toSharedFrameHeader();
        header.state = 1;
        header.frameIndex = frameIndex;
        if (header.timestampNs == 0)
        {
            header.timestampNs =
                static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }
        memcpy(slot + sizeof(header.state),
               reinterpret_cast<const uchar*>(&header) + sizeof(header.state),
               sizeof(header) - sizeof(header.state));
        memcpy(slot + kSharedFrameHeaderSize,
               frame.bytes.constData(),
               static_cast<size_t>(frame.bytes.size()));

        auto& stateValue = *reinterpret_cast<quint32*>(slot);
        std::atomic_ref<quint32>(stateValue).store(2, std::memory_order_release);
        auto* const control = reinterpret_cast<SharedMemoryControl*>(base);
        std::atomic_ref<quint32>(control->latestSlotIndex)
            .store(static_cast<quint32>(slotIndex), std::memory_order_release);
        if (frameIndexOut)
        {
            *frameIndexOut = frameIndex;
        }
        return true;
    }

    void ProviderDriverRuntime::emitPreviewStateEvent(const QString& deviceId, bool running)
    {
        QJsonObject event = makeEvent(driverhost::kEventPreviewState);
        event.insert(driverhost::kProviderIdField, m_providerId);
        event.insert(driverhost::kDeviceIdField, deviceId);
        event.insert(QStringLiteral("running"), running);
        emit eventReady(event);
    }

    void ProviderDriverRuntime::handleRequest(quint64 connectionId,
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
            response.insert(driverhost::kDevicesField, devices);
            if (m_stage)
            {
                response.insert(driverhost::kDefaultXYStageField, m_stage->defaultXYStage());
                response.insert(driverhost::kDefaultZStageField, m_stage->defaultZStage());
            }
            reply(response);
            return;
        }
        if (type == driverhost::kCommandShutdown)
        {
            stopForExit();
            reply(makeResponse(type, requestId, true));
            QTimer::singleShot(50, this, [this]() { emit shutdownRequested(); });
            return;
        }
        if (!m_provider)
        {
            reply(makeErrorResponse(type, requestId, QStringLiteral("Provider is not initialized")));
            return;
        }

        const bool configurationCommand =
            type == driverhost::kCommandListConfigGroups
            || type == driverhost::kCommandListConfigs
            || type == driverhost::kCommandGetCurrentConfig
            || type == driverhost::kCommandSetConfig;
        const HardwareDeviceDescriptor* const device =
            configurationCommand ? nullptr : requestedDevice(message);
        if (!configurationCommand && !device)
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
            if (mode == driverhost::kFrameDeliveryModePreviewLatest)
            {
                transport->deliveryMode.store(FrameDeliveryMode::PreviewLatest,
                                              std::memory_order_relaxed);
            }
            else if (mode == driverhost::kFrameDeliveryModeLatestOnly)
            {
                transport->deliveryMode.store(FrameDeliveryMode::LatestOnly,
                                              std::memory_order_relaxed);
                transport->previewTimer->stop();
                transport->previewFrame = {};
            }
            else if (mode == driverhost::kFrameDeliveryModeAllFrames)
            {
                transport->deliveryMode.store(FrameDeliveryMode::AllFrames,
                                              std::memory_order_relaxed);
                transport->previewTimer->stop();
                transport->previewFrame = {};
            }
            else
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Unknown frame delivery mode")));
                return;
            }
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("mode"), mode);
            reply(response);
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
            if (!transport
                || !m_camera->captureEventFrame(deviceId, frame, timeoutMs)
                || !writeFrame(*transport, frame, &frameIndex))
            {
                reply(makeErrorResponse(type,
                                        requestId,
                                        QStringLiteral("Provider failed to capture a frame")));
                return;
            }
            QJsonObject event = makeEvent(driverhost::kEventFrameAvailable);
            event.insert(driverhost::kProviderIdField, m_providerId);
            event.insert(driverhost::kDeviceIdField, deviceId);
            event.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
            emit eventReady(event);
            QJsonObject response = makeResponse(type, requestId, true);
            response.insert(QStringLiteral("frameIndex"), driverhost::encodeUInt64(frameIndex));
            reply(response);
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

        if (type == driverhost::kCommandListConfigGroups
            || type == driverhost::kCommandListConfigs
            || type == driverhost::kCommandGetCurrentConfig
            || type == driverhost::kCommandSetConfig)
        {
            if (!m_configuration)
            {
                unsupported(driverhost::kCapabilityConfiguration);
                return;
            }
            if (type == driverhost::kCommandListConfigGroups)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("groups"),
                                QJsonArray::fromStringList(
                                    m_configuration->availableConfigGroups()));
                reply(response);
                return;
            }
            const QString group = message.value(QStringLiteral("group")).toString();
            if (type == driverhost::kCommandListConfigs)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("configs"),
                                QJsonArray::fromStringList(
                                    m_configuration->availableConfigs(group)));
                reply(response);
                return;
            }
            if (type == driverhost::kCommandGetCurrentConfig)
            {
                QJsonObject response = makeResponse(type, requestId, true);
                response.insert(QStringLiteral("config"),
                                m_configuration->currentConfig(group));
                reply(response);
                return;
            }
            QString error;
            const bool ok = m_configuration->setConfig(
                group,
                message.value(QStringLiteral("config")).toString(),
                &error);
            reply(ok ? makeResponse(type, requestId, true)
                     : makeErrorResponse(type, requestId, error));
            return;
        }

        reply(makeErrorResponse(type, requestId, QStringLiteral("Unknown control command")));
    }

    void ProviderDriverRuntime::stopForExit()
    {
        m_acceptFrames.store(false, std::memory_order_release);
        for (const auto& transport : m_cameraTransports)
        {
            if (transport && transport->previewTimer)
            {
                transport->previewTimer->stop();
            }
        }
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
        for (const auto& transport : m_cameraTransports)
        {
            if (!transport) continue;
            QMutexLocker locker(&transport->frameMutex);
            transport->latestFrame = {};
            transport->latestDispatchQueued = false;
            transport->previewFrame = {};
        }
        m_camera = nullptr;
        m_properties = nullptr;
        m_stage = nullptr;
        m_shutter = nullptr;
        m_state = nullptr;
        m_configuration = nullptr;
        m_provider.reset();
        {
            QWriteLocker locker(&m_cameraTransportsLock);
            m_cameraTransports.clear();
        }
        m_devicesById.clear();
        m_devices.clear();
        if (m_pluginLoader)
        {
            m_pluginLoader->unload();
            m_pluginLoader.reset();
        }
    }

    class DriverHost final : public QObject
    {
        Q_OBJECT

    public:
        // Create the control server wrapper for one isolated runtime
        DriverHost(QString hostKey,
                   DriverHostRuntime* runtime,
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
        DriverHostRuntime* m_runtime{nullptr};
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
        connect(m_runtime, &DriverHostRuntime::responseReady,
                this, &DriverHost::onRuntimeResponse);
        connect(m_runtime, &DriverHostRuntime::eventReady,
                this, &DriverHost::broadcastEvent);
        connect(m_runtime, &DriverHostRuntime::shutdownRequested, this,
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
                    m_runtime, &DriverHostRuntime::handleRequest,
                    Qt::QueuedConnection);
            connect(connection, &ControlConnection::connectionClosed,
                    this, &DriverHost::onConnectionClosed);

            DriverHostRuntime* const runtime = m_runtime;
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

        DriverHostRuntime* const runtime = m_runtime;
        QMetaObject::invokeMethod(runtime,
                                  [runtime]() { runtime->stopForExit(); },
                                  Qt::BlockingQueuedConnection);
        m_runtimeThread.quit();
        m_runtimeThread.wait();
        m_runtime = nullptr;
    }
} // namespace scopeone::core::internal

// Launch one provider in an isolated DriverHost process
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("ScopeOne DriverHost");
    parser.addHelpOption();

    QCommandLineOption optProvider(QStringLiteral("provider"),
                                   QStringLiteral("Hardware provider ID"),
                                   QStringLiteral("id"));
    QCommandLineOption optDeviceId(QStringLiteral("deviceId"),
                                   QStringLiteral("Provider device ID"),
                                   QStringLiteral("id"));
    QCommandLineOption optPlugin(QStringLiteral("plugin"),
                                 QStringLiteral("Provider module path"),
                                 QStringLiteral("path"));
    QCommandLineOption optHostKey(QStringLiteral("hostKey"),
                                  QStringLiteral("DriverHost instance key"),
                                  QStringLiteral("key"));
    QCommandLineOption optProviderOption(QStringLiteral("option"),
                                         QStringLiteral("JSON provider options"),
                                         QStringLiteral("json"));
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

    parser.addOption(optProvider);
    parser.addOption(optDeviceId);
    parser.addOption(optPlugin);
    parser.addOption(optHostKey);
    parser.addOption(optProviderOption);
    parser.addOption(optAdapter);
    parser.addOption(optDevice);
    parser.addOption(optShm);
    parser.addOption(optExp);
    parser.addOption(optPreInit);
    parser.addOption(optProperty);
    parser.addOption(optAuto);
    parser.process(app);

    if (!parser.isSet(optProvider))
    {
        qCritical().noquote()
            << "Missing required argument: --provider";
        return 2;
    }

    const QString providerId = parser.value(optProvider).trimmed();
    const QString deviceId = parser.value(optDeviceId).trimmed();
    if (providerId.isEmpty())
    {
        qCritical().noquote() << "Provider ID cannot be empty";
        return 2;
    }

    const double exposureMs = parser.isSet(optExp)
                                  ? parser.value(optExp).toDouble()
                                  : 0.0;
    scopeone::core::internal::DriverHostRuntime* runtime = nullptr;
    if (providerId == QStringLiteral("micro-manager"))
    {
        if (deviceId.isEmpty()
            || !parser.isSet(optAdapter)
            || !parser.isSet(optDevice)
            || !parser.isSet(optShm))
        {
            qCritical().noquote()
                << "Micro-Manager requires --deviceId, --adapter, --device and --shm";
            return 2;
        }
        runtime = new scopeone::core::internal::MicroManagerDriverRuntime(
            providerId,
            deviceId,
            parser.value(optAdapter),
            parser.value(optDevice),
            parser.value(optShm),
            parser.values(optPreInit),
            parser.values(optProperty),
            exposureMs,
            parser.isSet(optAuto));
    }
    else
    {
        const QString pluginPath = parser.value(optPlugin).trimmed();
        const QString hostKey = parser.value(optHostKey).trimmed();
        if (pluginPath.isEmpty() || hostKey.isEmpty())
        {
            qCritical().noquote() << "External providers require --plugin and --hostKey";
            return 2;
        }

        QJsonObject providerOptions;
        for (const QString& encodedOptions : parser.values(optProviderOption))
        {
            QJsonParseError parseError;
            const QJsonDocument document =
                QJsonDocument::fromJson(encodedOptions.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                qCritical().noquote() << QStringLiteral("Invalid --option JSON: %1")
                                            .arg(parseError.errorString());
                return 2;
            }
            const QJsonObject object = document.object();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            {
                providerOptions.insert(it.key(), it.value());
            }
        }

        runtime = new scopeone::core::internal::ProviderDriverRuntime(
            pluginPath,
            providerId,
            hostKey,
            providerOptions);
    }

    const QString serverKey = providerId == QStringLiteral("micro-manager")
                                  ? deviceId
                                  : parser.value(optHostKey).trimmed();
    scopeone::core::internal::DriverHost driverHost(serverKey, runtime);
    if (!driverHost.start())
    {
        return 2;
    }

    return app.exec();
}

#include "DriverHostMain.moc"
