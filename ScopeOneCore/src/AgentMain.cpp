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

#include <cmath>
#include <cstring>
#include <memory>

#include "MMCore.h"
#include "internal/AgentProtocol.h"
#include "scopeone/SharedFrame.h"

namespace scopeone::core::internal
{
    using scopeone::core::SharedFrameHeader;
    using scopeone::core::SharedMemoryControl;
    using scopeone::core::SharedPixelFormat;
    using scopeone::core::computeMaxFrameBytes;
    using scopeone::core::kSharedFrameHeaderSize;
    using scopeone::core::kSharedFrameMaxBytes;
    using scopeone::core::kSharedFrameNumSlots;
    using scopeone::core::kSharedFrameSlotStride;
    using scopeone::core::kSharedMemoryControlSize;

    class ControlConnection final : public QObject
    {
        Q_OBJECT

    public:
        ControlConnection(quint64 connectionId, QLocalSocket* socket, QObject* parent = nullptr)
            : QObject(parent)
              , m_connectionId(connectionId)
              , m_socket(socket)
        {
            Q_ASSERT(m_socket);
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

        void sendMessage(const QJsonObject& message)
        {
            if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
            {
                return;
            }
            m_socket->write(agent::encodeMessage(message));
            m_socket->flush();
        }

    signals:
        void requestReceived(quint64 connectionId,
                             quint64 requestId,
                             const QString& type,
                             const QJsonObject& message);
        void connectionClosed(quint64 connectionId);

    private slots:
        void onReadyRead()
        {
            if (!m_socket)
            {
                return;
            }

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
        AgentRuntime(QString cameraId,
                     QString adapter,
                     QString device,
                     QString shmKey,
                     QStringList preInitProperties,
                     double exposureMs,
                     bool autoPreview,
                     QObject* parent = nullptr)
            : QObject(parent)
              , m_cameraId(std::move(cameraId))
              , m_adapter(std::move(adapter))
              , m_device(std::move(device))
              , m_shmKey(std::move(shmKey))
              , m_preInitProperties(std::move(preInitProperties))
              , m_exposureMs(exposureMs)
              , m_autoPreview(autoPreview)
        {
        }

        QString lastError() const
        {
            return m_lastError;
        }

        bool initializeRuntime()
        {
            m_lastError.clear();
            setState(State::Starting);

            if (!m_timer)
            {
                m_timer = new QTimer(this);
                m_timer->setInterval(1);
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
                if (!applyPreInitProperties(label, &preInitError))
                {
                    m_lastError = preInitError;
                    setState(State::Error, m_lastError);
                    return false;
                }
                m_mmcore->initializeDevice(label.c_str());
                m_mmcore->setCameraDevice(label.c_str());

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
                    finalExposure = m_exposureMs;
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
                memset(m_shm->data(), 0, totalBytes);
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
        enum class State
        {
            Starting,
            Idle,
            Previewing,
            Error,
            ShuttingDown
        };

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
        bool applyPreInitProperties(const std::string& label, QString* errorMessage);
        void emitPreviewStateEvent();
        void emitAgentErrorEvent(const QString& error);
        void emitBufferOverflowEvent(long bufferCapacity);
        void emitFrameAvailableEvent(quint64 frameIndex);
        bool startPreviewInternal(QString* errorMessage);
        bool stopPreviewInternal(QString* errorMessage);
        bool captureEventFrameInternal(quint64& frameIndex, QString* errorMessage);
        bool writeFrameToSharedMemory(const void* pixels, quint64* frameIndexOut = nullptr);
        void pollAndWrite();
        bool ensureFrameLayout(unsigned width, unsigned height, unsigned bytesPerPixel);

        QString m_cameraId;
        QString m_adapter;
        QString m_device;
        QString m_shmKey;
        QStringList m_preInitProperties;
        double m_exposureMs{10.0};
        bool m_autoPreview{false};

        QString m_lastError;
        State m_state{State::Starting};

        std::unique_ptr<CMMCore> m_mmcore;
        std::unique_ptr<QSharedMemory> m_shm;
        QTimer* m_timer{nullptr};

        FrameLayout m_frameLayout{};
        bool m_frameLayoutValid{false};
        bool m_loggedOversizedFrame{false};
        bool m_loggedUnsupportedFormat{false};

        quint64 m_frameIndex{0};
        int m_overflowCheckCounter{0};
    };

    void AgentRuntime::publishHello()
    {
        emit eventReady(makeEvent(agent::kEventHello));
    }

    bool AgentRuntime::applyPreInitProperties(const std::string& label, QString* errorMessage)
    {
        for (const QString& encodedProperty : m_preInitProperties)
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
                    *errorMessage = QStringLiteral("Invalid pre-init property payload for '%1'")
                        .arg(m_cameraId);
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
                    *errorMessage = QStringLiteral("Missing pre-init property name for '%1'")
                        .arg(m_cameraId);
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
                    *errorMessage = QString("Failed to apply pre-init property '%1': %2")
                        .arg(propertyName, QString::fromStdString(mmError.getMsg()));
                }
                return false;
            }
        }

        return true;
    }

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
                    if (std::abs(exposureMs - m_exposureMs) > 0.0005)
                    {
                        m_mmcore->setExposure(exposureMs);
                        m_exposureMs = exposureMs;
                    }
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
            QString value;
            QString propertyType = QStringLiteral("Unknown");
            bool readOnly = true;
            QJsonArray allowedValues;
            bool hasLimits = false;
            double lowerLimit = 0.0;
            double upperLimit = 0.0;
            QString error;
            bool ok = true;

            try
            {
                value = QString::fromStdString(
                    m_mmcore->getProperty(m_cameraId.toStdString().c_str(),
                                          name.toStdString().c_str()));

                try
                {
                    switch (m_mmcore->getPropertyType(m_cameraId.toStdString().c_str(),
                                                      name.toStdString().c_str()))
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
                    readOnly = m_mmcore->isPropertyReadOnly(
                        m_cameraId.toStdString().c_str(),
                        name.toStdString().c_str());
                }
                catch (const CMMError&)
                {
                }

                try
                {
                    const auto values =
                        m_mmcore->getAllowedPropertyValues(m_cameraId.toStdString().c_str(),
                                                           name.toStdString().c_str());
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
                    hasLimits = m_mmcore->hasPropertyLimits(m_cameraId.toStdString().c_str(),
                                                            name.toStdString().c_str());
                    if (hasLimits)
                    {
                        lowerLimit = m_mmcore->getPropertyLowerLimit(
                            m_cameraId.toStdString().c_str(),
                            name.toStdString().c_str());
                        upperLimit = m_mmcore->getPropertyUpperLimit(
                            m_cameraId.toStdString().c_str(),
                            name.toStdString().c_str());
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
                m_mmcore->setROI(x, y, width, height);
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
                m_mmcore->clearROI();
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
                m_mmcore->getROI(x, y, width, height);
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

    void AgentRuntime::stopForExit()
    {
        stopPreviewInternal(nullptr);
        setState(State::ShuttingDown);
    }

    bool AgentRuntime::previewRunning() const
    {
        return m_state == State::Previewing;
    }

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

    QJsonObject AgentRuntime::makeResponse(const QString& type, quint64 requestId, bool ok) const
    {
        QJsonObject response =
            agent::makeEnvelope(agent::kMessageKindResponse, type, requestId);
        response.insert(QStringLiteral("ok"), ok);
        return response;
    }

    QJsonObject AgentRuntime::makeErrorResponse(const QString& type,
                                                quint64 requestId,
                                                const QString& error) const
    {
        QJsonObject response = makeResponse(type, requestId, false);
        response.insert(QStringLiteral("error"), error);
        return response;
    }

    QJsonObject AgentRuntime::makeEvent(const QString& type) const
    {
        return agent::makeEnvelope(agent::kMessageKindEvent, type);
    }

    void AgentRuntime::emitPreviewStateEvent()
    {
        QJsonObject event = makeEvent(agent::kEventPreviewState);
        event.insert(QStringLiteral("running"), previewRunning());
        emit eventReady(event);
    }

    void AgentRuntime::emitAgentErrorEvent(const QString& error)
    {
        QJsonObject event = makeEvent(agent::kEventAgentError);
        event.insert(QStringLiteral("error"), error);
        emit eventReady(event);
    }

    void AgentRuntime::emitBufferOverflowEvent(long bufferCapacity)
    {
        QJsonObject event = makeEvent(agent::kEventBufferOverflow);
        event.insert(QStringLiteral("capacityFrames"), static_cast<int>(bufferCapacity));
        emit eventReady(event);
    }

    void AgentRuntime::emitFrameAvailableEvent(quint64 frameIndex)
    {
        QJsonObject event = makeEvent(agent::kEventFrameAvailable);
        event.insert(QStringLiteral("frameIndex"), agent::encodeUInt64(frameIndex));
        emit eventReady(event);
    }

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
            m_mmcore->startContinuousSequenceAcquisition(0.0);
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

            if (!writeFrameToSharedMemory(pixels, &frameIndex))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Shared memory unavailable");
                }
                return false;
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

    bool AgentRuntime::writeFrameToSharedMemory(const void* pixels, quint64* frameIndexOut)
    {
        if (!m_shm || !m_shm->isAttached())
        {
            return false;
        }
        if (!m_shm->lock())
        {
            return false;
        }

        uchar* base = static_cast<uchar*>(m_shm->data());
        if (!base)
        {
            m_shm->unlock();
            return false;
        }

        const int slotIndex = static_cast<int>(m_frameIndex % kSharedFrameNumSlots);
        auto* control = reinterpret_cast<SharedMemoryControl*>(base);
        uchar* ptr = base + kSharedMemoryControlSize + slotIndex * kSharedFrameSlotStride;
        auto* header = reinterpret_cast<SharedFrameHeader*>(ptr);

        header->state = 1;
        header->width = m_frameLayout.width;
        header->height = m_frameLayout.height;
        header->stride = m_frameLayout.stride;
        header->pixelFormat = static_cast<quint32>(m_frameLayout.format);
        header->bitsPerSample = m_frameLayout.bitDepth;
        header->channels = m_frameLayout.channels;
        header->frameIndex = ++m_frameIndex;
        header->timestampNs =
            static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;

        uchar* dst = ptr + kSharedFrameHeaderSize;
        memcpy(dst, pixels, static_cast<size_t>(m_frameLayout.byteCount));

        header->state = 2;
        control->latestSlotIndex = static_cast<quint32>(slotIndex);
        if (frameIndexOut)
        {
            *frameIndexOut = m_frameIndex;
        }

        m_shm->unlock();
        return true;
    }

    void AgentRuntime::pollAndWrite()
    {
        if (!m_mmcore)
        {
            return;
        }

        try
        {
            long remaining = m_mmcore->getRemainingImageCount();

            if (++m_overflowCheckCounter >= 100 || remaining > 10)
            {
                m_overflowCheckCounter = 0;
                if (m_mmcore->isBufferOverflowed())
                {
                    emitBufferOverflowEvent(m_mmcore->getBufferTotalCapacity());
                }
            }

            if (remaining <= 0)
            {
                return;
            }

            quint64 newestFrameIndex = 0;
            while (remaining-- > 0)
            {
                const void* pixels = m_mmcore->popNextImage();
                if (!pixels)
                {
                    break;
                }

                const unsigned width = m_mmcore->getImageWidth();
                const unsigned height = m_mmcore->getImageHeight();
                const unsigned bytesPerPixel = m_mmcore->getBytesPerPixel();
                if (!ensureFrameLayout(width, height, bytesPerPixel))
                {
                    break;
                }

                quint64 writtenFrameIndex = 0;
                if (!writeFrameToSharedMemory(pixels, &writtenFrameIndex))
                {
                    break;
                }
                newestFrameIndex = writtenFrameIndex;
            }

            if (newestFrameIndex != 0)
            {
                emitFrameAvailableEvent(newestFrameIndex);
            }
        }
        catch (const CMMError& error)
        {
            const QString message =
                QStringLiteral("Agent capture error: %1")
                .arg(QString::fromStdString(error.getMsg()));
            setState(State::Error, message);
        }
    }

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
        updated.stride = width * bytesPerPixel;
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
            bitDepth = static_cast<quint16>(m_mmcore->getImageBitDepth());
        }
        catch (const CMMError&)
        {
            bitDepth =
                (updated.format == SharedPixelFormat::Mono16) ? 16 : 8;
        }
        if (bitDepth < 1 || bitDepth > 16)
        {
            bitDepth =
                (updated.format == SharedPixelFormat::Mono16) ? 16 : 8;
        }
        updated.bitDepth = bitDepth;

        m_frameLayout = updated;
        m_frameLayoutValid = true;
        m_loggedOversizedFrame = false;
        return true;
    }

    class Agent final : public QObject
    {
        Q_OBJECT

    public:
        Agent(QString cameraId,
              QString adapter,
              QString device,
              QString shmKey,
              QStringList preInitProperties,
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
              , m_exposureMs(exposureMs)
              , m_autoPreview(autoPreview)
        {
            qRegisterMetaType<QJsonObject>("QJsonObject");
            qRegisterMetaType<quint64>("quint64");
        }

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
        double m_exposureMs{0.0};
        bool m_autoPreview{false};

        std::unique_ptr<QLocalServer> m_ctrlServer;
        QThread m_runtimeThread;
        AgentRuntime* m_runtime{nullptr};
        quint64 m_nextConnectionId{1};
        QHash<quint64, ControlConnection*> m_connections;
    };

    bool Agent::start()
    {
        m_runtime = new AgentRuntime(m_cameraId,
                                     m_adapter,
                                     m_device,
                                     m_shmKey,
                                     m_preInitProperties,
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

    void Agent::onRuntimeResponse(quint64 connectionId, const QJsonObject& response)
    {
        auto it = m_connections.find(connectionId);
        if (it == m_connections.end() || !it.value())
        {
            return;
        }
        it.value()->sendMessage(response);
    }

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

    void Agent::onConnectionClosed(quint64 connectionId)
    {
        m_connections.remove(connectionId);
    }

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
    QCommandLineOption optAuto(QStringLiteral("autoPreview"),
                               QStringLiteral("Start preview immediately"));

    parser.addOption(optCamId);
    parser.addOption(optAdapter);
    parser.addOption(optDevice);
    parser.addOption(optShm);
    parser.addOption(optExp);
    parser.addOption(optPreInit);
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
