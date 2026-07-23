#include "internal/CameraBackend.h"
#include "internal/AgentProtocol.h"
#include "scopeone/SharedFrame.h"

#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QDebug>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QProcess>
#include <QSharedMemory>
#include <QTimer>
#include <atomic>
#include <cstring>
#include <memory>
#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>
#include <QLocalSocket>

namespace scopeone::core::internal
{
    static_assert(std::atomic_ref<quint32>::is_always_lock_free,
                  "Shared frame state requires lock-free 32-bit atomics");

    using scopeone::core::ImageFrame;
    using scopeone::core::SharedFrameHeader;
    using scopeone::core::SharedMemoryControl;
    using scopeone::core::SharedPixelFormat;
    using scopeone::core::kSharedFrameHeaderSize;
    using scopeone::core::kSharedFrameMaxBytes;
    using scopeone::core::kSharedFrameNumSlots;
    using scopeone::core::kSharedFrameSlotStride;
    using scopeone::core::kSharedMemoryControlSize;

    namespace
    {
        constexpr int kAgentControlReadyTimeoutMs = 15000;

        enum class AgentFrameDeliveryMode
        {
            PreviewLatest,
            LatestOnly,
            AllFrames
        };

        // Normalizes camera ids before they become backend keys
        QString normalizedCameraId(const QString& cameraId)
        {
            return cameraId.trimmed();
        }

        const QString& frameDeliveryModeName(AgentFrameDeliveryMode mode)
        {
            switch (mode)
            {
            case AgentFrameDeliveryMode::PreviewLatest:
                return agent::kFrameDeliveryModePreviewLatest;
            case AgentFrameDeliveryMode::LatestOnly:
                return agent::kFrameDeliveryModeLatestOnly;
            case AgentFrameDeliveryMode::AllFrames:
                return agent::kFrameDeliveryModeAllFrames;
            }
            return agent::kFrameDeliveryModePreviewLatest;
        }

        AgentFrameDeliveryMode nonRecordingDeliveryMode(bool highRate)
        {
            return highRate
                       ? AgentFrameDeliveryMode::LatestOnly
                       : AgentFrameDeliveryMode::PreviewLatest;
        }

    } // namespace

    struct AgentControlSession final : QObject
    {
        struct PendingRequest
        {
            QByteArray encoded;
            QTimer* timer{nullptr};
            std::function<void(bool, const QJsonObject&, const QString&)> completion;
        };

        explicit AgentControlSession(const QString& cameraId,
                                     const QString& serverName,
                                     QObject* parent = nullptr)
            : QObject(parent)
              , m_cameraId(cameraId)
              , m_serverName(serverName)
        {
            m_reconnectTimer.setSingleShot(true);
            connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() { ensureConnected(); });

            connect(&m_socket, &QLocalSocket::connected, this, [this]() { flushPendingWrites(); });
            connect(&m_socket, &QLocalSocket::disconnected, this, [this]()
            {
                m_readBuffer.clear();
                setReady(false);
                failAll(QStringLiteral("Control session disconnected"));
                if (!m_closing)
                {
                    m_reconnectTimer.start(100);
                }
            });
            connect(&m_socket, &QLocalSocket::readyRead, this, [this]() { handleReadyRead(); });
            connect(&m_socket, &QLocalSocket::errorOccurred, this,
                    [this](QLocalSocket::LocalSocketError)
                    {
                        if (m_closing)
                        {
                            return;
                        }
                        setReady(false);
                        failAll(QStringLiteral("Control socket error for '%1'").arg(m_cameraId));
                        if (m_socket.state() == QLocalSocket::UnconnectedState)
                        {
                            m_reconnectTimer.start(100);
                        }
                    });
        }

        void start()
        {
            ensureConnected();
        }

        void stop()
        {
            m_closing = true;
            m_reconnectTimer.stop();
            m_readBuffer.clear();
            setReady(false);
            failAll(QStringLiteral("Control session closed"));
            if (m_socket.state() != QLocalSocket::UnconnectedState)
            {
                m_socket.abort();
            }
        }

        bool isReady() const
        {
            return m_ready && m_socket.state() == QLocalSocket::ConnectedState;
        }

        bool waitForReady(int timeoutMs)
        {
            if (isReady())
            {
                return true;
            }

            ensureConnected();

            QEventLoop loop;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            watchdog.setInterval((std::max)(1, timeoutMs));

            const auto quitIfReady = [this, &loop]()
            {
                if (isReady())
                {
                    loop.quit();
                }
            };

            connect(&m_socket, &QLocalSocket::connected, &loop, quitIfReady);
            connect(&m_socket, &QLocalSocket::readyRead, &loop, quitIfReady);
            connect(&m_socket, &QLocalSocket::disconnected, &loop, quitIfReady);
            connect(&m_socket, &QLocalSocket::errorOccurred, &loop,
                    [quitIfReady](QLocalSocket::LocalSocketError)
                    {
                        quitIfReady();
                    });
            connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

            watchdog.start();
            if (!isReady())
            {
                loop.exec();
            }
            return isReady();
        }

        void addReadyHandler(std::function<void(bool)> handler)
        {
            m_readyHandlers.push_back(std::move(handler));
        }

        void addEventHandler(std::function<void(const QJsonObject&)> handler)
        {
            m_eventHandlers.push_back(std::move(handler));
        }

        bool sendRequest(const QJsonObject& request,
                         int timeoutMs,
                         std::function<void(bool, const QJsonObject&, const QString&)> completion)
        {
            const QString type = request.value(agent::kMessageTypeField).toString();
            if (type.isEmpty())
            {
                return false;
            }

            const quint64 requestId = m_nextRequestId++;
            QJsonObject envelope = request;
            envelope.insert(agent::kEnvelopeKindField, agent::kMessageKindRequest);
            envelope.insert(agent::kEnvelopeVersionField, static_cast<int>(agent::kProtocolVersion));
            envelope.insert(agent::kEnvelopeRequestIdField, agent::encodeUInt64(requestId));

            PendingRequest pending;
            pending.encoded = agent::encodeMessage(envelope);
            pending.completion = std::move(completion);
            pending.timer = new QTimer(this);
            pending.timer->setSingleShot(true);
            connect(pending.timer, &QTimer::timeout, this, [this, requestId]()
            {
                completeRequest(requestId, false, QJsonObject{}, QStringLiteral("Control request timed out"));
            });
            pending.timer->start((std::max)(1, timeoutMs));

            m_pending.insert(requestId, pending);
            m_sendQueue.push_back(requestId);
            ensureConnected();
            flushPendingWrites();
            return true;
        }

    private:
        void ensureConnected()
        {
            if (m_closing)
            {
                return;
            }
            if (m_socket.state() == QLocalSocket::ConnectedState
                || m_socket.state() == QLocalSocket::ConnectingState)
            {
                return;
            }
            m_socket.connectToServer(m_serverName);
        }

        void setReady(bool ready)
        {
            if (m_ready == ready)
            {
                return;
            }
            m_ready = ready;
            for (const auto& handler : m_readyHandlers)
            {
                handler(m_ready);
            }
        }

        void flushPendingWrites()
        {
            if (m_socket.state() != QLocalSocket::ConnectedState)
            {
                return;
            }

            while (!m_sendQueue.isEmpty())
            {
                const quint64 requestId = m_sendQueue.front();
                m_sendQueue.pop_front();

                auto it = m_pending.find(requestId);
                if (it == m_pending.end())
                {
                    continue;
                }
                m_socket.write(it->encoded);
            }
            m_socket.flush();
        }

        void handleReadyRead()
        {
            m_readBuffer += m_socket.readAll();
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
                    resetWithError(error);
                    return;
                }

                if (message.value(agent::kEnvelopeVersionField).toInt(0)
                    != static_cast<int>(agent::kProtocolVersion))
                {
                    resetWithError(QStringLiteral("Control protocol version mismatch"));
                    return;
                }

                const QString kind = message.value(agent::kEnvelopeKindField).toString();
                if (kind == agent::kMessageKindResponse)
                {
                    const quint64 requestId =
                        agent::decodeUInt64(message.value(agent::kEnvelopeRequestIdField));
                    if (requestId == 0)
                    {
                        continue;
                    }
                    completeRequest(requestId, true, message, QString{});
                    continue;
                }

                if (kind == agent::kMessageKindEvent)
                {
                    const QString type = message.value(agent::kMessageTypeField).toString();
                    if (type == agent::kEventHello)
                    {
                        setReady(true);
                    }
                    for (const auto& handler : m_eventHandlers)
                    {
                        handler(message);
                    }
                }
            }
        }

        void completeRequest(quint64 requestId,
                             bool ok,
                             const QJsonObject& response,
                             const QString& error)
        {
            auto it = m_pending.find(requestId);
            if (it == m_pending.end())
            {
                return;
            }

            PendingRequest pending = it.value();
            m_pending.erase(it);

            if (pending.timer)
            {
                pending.timer->stop();
                pending.timer->deleteLater();
            }
            if (pending.completion)
            {
                pending.completion(ok, response, error);
            }
        }

        void failAll(const QString& error)
        {
            const QList<quint64> requestIds = m_pending.keys();
            for (quint64 requestId : requestIds)
            {
                completeRequest(requestId, false, QJsonObject{}, error);
            }
            m_sendQueue.clear();
        }

        void resetWithError(const QString& error)
        {
            m_readBuffer.clear();
            setReady(false);
            failAll(error);
            if (m_socket.state() != QLocalSocket::UnconnectedState)
            {
                m_socket.abort();
            }
            if (!m_closing)
            {
                m_reconnectTimer.start(100);
            }
        }

        QString m_cameraId;
        QString m_serverName;
        QLocalSocket m_socket;
        QTimer m_reconnectTimer;
        QByteArray m_readBuffer;
        QMap<quint64, PendingRequest> m_pending;
        QList<quint64> m_sendQueue;
        QList<std::function<void(bool)>> m_readyHandlers;
        QList<std::function<void(const QJsonObject&)>> m_eventHandlers;
        quint64 m_nextRequestId{1};
        bool m_ready{false};
        bool m_closing{false};
    };

    class AgentCameraBackend;

    class AgentFrameWorker final : public QObject
    {
    public:
        explicit AgentFrameWorker(AgentCameraBackend* owner)
            : m_owner(owner)
        {
        }

        void addCamera(const QString& cameraId, const QString& shmKey);
        void removeCamera(const QString& cameraId);
        void clear();
        bool ensureSharedMemory(const QString& cameraId);
        ImageFrame consumeFrames(const QString& cameraId);
        void consumeFramesAsync(const QString& cameraId);

    private:
        struct ReaderSlot
        {
            QString cameraId;
            QString shmKey;
            std::unique_ptr<QSharedMemory> shm;
            quint64 lastFrameIndex{0};
            ImageFrame latestFrame;
        };

        bool ensureSharedMemory(ReaderSlot& slot);
        bool copyFrame(ReaderSlot& slot,
                       const SharedFrameHeader& header,
                       const uchar* pixelData,
                       QList<ImageFrame>& frames);
        bool readLatestFrame(ReaderSlot& slot,
                             QList<ImageFrame>& frames,
                             quint64& acquiredFrameCount);
        bool readAllFrames(ReaderSlot& slot,
                           QList<ImageFrame>& frames,
                           quint64& acquiredFrameCount);

        AgentCameraBackend* const m_owner;
        QMap<QString, std::shared_ptr<ReaderSlot>> m_readers;
    };

    struct AgentCameraSlot
    {
        QString cameraId;
        QString shmKey;
        std::shared_ptr<QProcess> process;
        std::shared_ptr<AgentControlSession> control;
        bool isRunning{false};
        double exposureMs{10.0};
        bool frameReadQueued{false};
        bool frameReadRequested{false};
    };

    class AgentCameraBackend final : public CameraBackend
    {
    public:
        AgentCameraBackend();
        ~AgentCameraBackend() override;

        Kind kind() const override { return Kind::Agent; }
        bool addAgentCamera(const QString& cameraId,
                            const QString& adapter,
                            const QString& device,
                            const QStringList& preInitProperties,
                            const QStringList& properties,
                            double exposureMs) override;
        void removeAgentCamera(const QString& cameraId);
        bool isPreviewRunning(const QString& cameraId) const override
        {
            const auto it = m_cameras.constFind(cameraId);
            return it != m_cameras.constEnd() && it.value() && it.value()->isRunning;
        }
        void setFrameDeliveryPaused(bool paused) override;
        bool setRecordingFrameDeliveryEnabled(bool enabled) override;
        bool setHighRateFrameDeliveryEnabled(bool enabled) override;
        bool captureEventFrame(const QString& cameraId,
                               ImageFrame& frame,
                               int timeoutMs) override;

        void shutdown();

        bool startPreview() override
        {
            if (m_cameras.isEmpty())
            {
                return false;
            }

            QStringList startedCameraIds;
            for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
            {
                const QString cameraId = it.key();
                if (it.value() && it.value()->isRunning)
                {
                    continue;
                }
                if (!startPreviewFor(cameraId))
                {
                    for (const QString& startedCameraId : startedCameraIds)
                    {
                        stopPreviewFor(startedCameraId);
                    }
                    return false;
                }
                startedCameraIds.append(cameraId);
            }
            qInfo().noquote() << "Agent preview started";
            return true;
        }

        bool stopPreview() override
        {
            bool ok = true;
            for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
            {
                ok = stopPreviewFor(it.key()) && ok;
            }
            if (ok)
            {
                qInfo().noquote() << "Agent preview stopped";
            }
            return ok;
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            if (!m_cameras.contains(cameraId))
            {
                qWarning().noquote() << QString("Camera '%1' not found").arg(cameraId);
                return false;
            }

            AgentCameraSlot& slot = *m_cameras[cameraId];
            if (slot.isRunning)
            {
                return true;
            }
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandStartPreview);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200))
            {
                qWarning().noquote() << QString("Failed to connect to camera '%1'").arg(cameraId);
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                qWarning().noquote() << QString("Agent refused to start preview for '%1'").arg(cameraId);
                return false;
            }

            bool sharedMemoryReady = false;
            for (int attempt = 0; attempt < 20 && !sharedMemoryReady; ++attempt)
            {
                sharedMemoryReady = prepareFrameReader(cameraId);
                if (!sharedMemoryReady)
                {
                    QThread::msleep(50);
                }
            }
            if (!sharedMemoryReady)
            {
                qWarning().noquote() << QString("Shared memory unavailable for '%1'").arg(cameraId);
                QJsonObject stopRequest;
                stopRequest.insert(agent::kMessageTypeField, agent::kCommandStopPreview);
                sendControlCommand(cameraId, stopRequest, nullptr, 1200);
                return false;
            }

            slot.isRunning = true;
            notifyPreviewStarted(cameraId);
            return true;
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            const auto it = m_cameras.find(cameraId);
            if (it == m_cameras.end() || !it.value())
            {
                return false;
            }
            if (!it.value()->isRunning)
            {
                return true;
            }

            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandStopPreview);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200))
            {
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                return false;
            }

            const bool wasRunning = it.value()->isRunning;
            it.value()->isRunning = false;
            if (wasRunning)
            {
                notifyPreviewStopped();
            }
            return true;
        }

    protected:
        bool hasRunningCamera() const override
        {
            for (auto it = m_cameras.constBegin(); it != m_cameras.constEnd(); ++it)
            {
                if (it.value() && it.value()->isRunning)
                {
                    return true;
                }
            }
            return false;
        }

        bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const override
        {
            if (m_cameras.isEmpty())
            {
                return false;
            }

            const QString target =
                cameraIdOrAll.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0
                    ? m_cameras.firstKey()
                    : cameraIdOrAll;
            if (!m_cameras.contains(target))
            {
                return false;
            }
            cameraId = target;
            return true;
        }

        QStringList resolveTargetCameraIds(const QString& cameraIdOrAll) const override
        {
            if (cameraIdOrAll.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
            {
                return m_cameras.keys();
            }
            return m_cameras.contains(cameraIdOrAll) ? QStringList{cameraIdOrAll} : QStringList{};
        }

        bool readExposureFor(const QString& cameraId, double& exposureMs) const override
        {
            const auto it = m_cameras.constFind(cameraId);
            if (it == m_cameras.constEnd() || !it.value())
            {
                return false;
            }
            exposureMs = it.value()->exposureMs;
            return exposureMs > 0.0;
        }

        bool writeExposureFor(const QString& cameraId, double exposureMs) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandSetExposure);
            req.insert(QStringLiteral("value"), exposureMs);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200)
                || !resp.value(QStringLiteral("ok")).toBool(false))
            {
                return false;
            }
            const double actualExposureMs = resp.value(QStringLiteral("exposureMs")).toDouble(exposureMs);
            m_cameras[cameraId]->exposureMs = actualExposureMs;
            return true;
        }

        QStringList listPropertiesFor(const QString& cameraId) override
        {
            QStringList out;
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandListProperties);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 4000))
            {
                return out;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                return out;
            }
            const QJsonArray properties = resp.value(QStringLiteral("properties")).toArray();
            for (const auto& value : properties)
            {
                out << value.toString();
            }
            return out;
        }

        bool readPropertyDetailsFor(const QString& cameraId,
                                    const QString& name,
                                    bool fromCache,
                                    CameraPropertyReadback& readback) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandGetProperty);
            req.insert(QStringLiteral("name"), name);
            req.insert(QStringLiteral("fromCache"), fromCache);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 4000))
            {
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                return false;
            }

            readback.value = resp.value(QStringLiteral("value")).toString();
            readback.type = resp.value(QStringLiteral("propertyType")).toString(QStringLiteral("Unknown"));
            readback.readOnly = resp.value(QStringLiteral("readOnly")).toBool(true);
            readback.preInit = resp.value(QStringLiteral("preInit")).toBool(false);
            readback.hasLimits = resp.value(QStringLiteral("hasLimits")).toBool(false);
            readback.lowerLimit = resp.value(QStringLiteral("lowerLimit")).toDouble(0.0);
            readback.upperLimit = resp.value(QStringLiteral("upperLimit")).toDouble(0.0);

            const QJsonArray allowedValues = resp.value(QStringLiteral("allowedValues")).toArray();
            for (const auto& value : allowedValues)
            {
                readback.allowedValues << value.toString();
            }
            return true;
        }

        bool setPropertyFor(const QString& cameraId,
                            const QString& name,
                            const QString& value,
                            QString* errorMessage) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandSetProperty);
            req.insert(QStringLiteral("name"), name);
            req.insert(QStringLiteral("value"), value);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 4000))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Agent control request failed");
                }
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                if (errorMessage)
                {
                    *errorMessage = resp.value(QStringLiteral("error")).toString();
                }
                return false;
            }
            return true;
        }

        bool setROIFor(const QString& cameraId, int x, int y, int width, int height) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandSetRoi);
            req.insert(QStringLiteral("x"), x);
            req.insert(QStringLiteral("y"), y);
            req.insert(QStringLiteral("width"), width);
            req.insert(QStringLiteral("height"), height);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200))
            {
                qWarning().noquote() << QString("Failed to connect to camera '%1'").arg(cameraId);
                return false;
            }
            const bool ok = resp.value(QStringLiteral("ok")).toBool(false);
            if (!ok)
            {
                qWarning().noquote() << QString("Failed to set ROI for '%1': %2")
                    .arg(cameraId, resp.value(QStringLiteral("error")).toString(QStringLiteral("Unknown error")));
            }
            return ok;
        }

        bool clearROIFor(const QString& cameraId) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandClearRoi);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200))
            {
                qWarning().noquote() << QString("Failed to connect to camera '%1'").arg(cameraId);
                return false;
            }
            const bool ok = resp.value(QStringLiteral("ok")).toBool(false);
            if (!ok)
            {
                qWarning().noquote() << QString("Failed to clear ROI for '%1': %2")
                    .arg(cameraId, resp.value(QStringLiteral("error")).toString(QStringLiteral("Unknown error")));
            }
            return ok;
        }

        bool getROIFor(const QString& cameraId, int& x, int& y, int& width, int& height) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandGetRoi);
            QJsonObject resp;
            if (!sendControlCommand(cameraId, req, &resp, 1200))
            {
                qWarning().noquote() << QString("Failed to connect to camera '%1'").arg(cameraId);
                return false;
            }
            const bool ok = resp.value(QStringLiteral("ok")).toBool(false);
            if (!ok)
            {
                qWarning().noquote() << QString("Failed to get ROI for '%1': %2")
                    .arg(cameraId, resp.value(QStringLiteral("error")).toString(QStringLiteral("Unknown error")));
                return false;
            }

            x = resp.value(QStringLiteral("x")).toInt(0);
            y = resp.value(QStringLiteral("y")).toInt(0);
            width = resp.value(QStringLiteral("width")).toInt(0);
            height = resp.value(QStringLiteral("height")).toInt(0);
            return true;
        }
    private:
        friend class AgentFrameWorker;

        bool waitForControlReady(AgentCameraSlot& slot, int timeoutMs);
        bool addFrameReader(const QString& cameraId, const QString& shmKey);
        void removeFrameReader(const QString& cameraId);
        bool prepareFrameReader(const QString& cameraId);
        ImageFrame consumeFrameNow(const QString& cameraId);
        void scheduleFrameRead(const QString& cameraId);
        void completeFrameRead(const QString& cameraId);
        bool sendControlCommand(const QString& cameraId,
                                const QJsonObject& request,
                                QJsonObject* response,
                                int timeoutMs);
        bool sendFrameDeliveryMode(const QString& cameraId, AgentFrameDeliveryMode mode);

        QMap<QString, std::shared_ptr<AgentCameraSlot>> m_cameras;
        std::atomic_bool m_frameDeliveryPaused{false};
        bool m_shuttingDown{false};
        QThread m_frameThread;
        AgentFrameWorker* m_frameWorker{nullptr};
    };

    // Returns payload byte count from a shared frame header
    static quint64 sharedFramePayloadSize(const SharedFrameHeader& header)
    {
        return static_cast<quint64>(header.stride) * header.height;
    }

    // Validates one shared frame header before reading pixels
    static bool headerLooksSane(const SharedFrameHeader& header)
    {
        if (header.channels != 1) return false;
        if (header.width == 0 || header.height == 0) return false;
        if (header.stride == 0) return false;
        if (header.pixelFormat != static_cast<quint32>(SharedPixelFormat::Mono8) &&
            header.pixelFormat != static_cast<quint32>(SharedPixelFormat::Mono16))
        {
            return false;
        }

        const quint32 bytesPerPixel =
            (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16)) ? 2u : 1u;
        if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono8)
            && header.bitsPerSample != 8)
        {
            return false;
        }
        if (header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16)
            && (header.bitsPerSample == 0 || header.bitsPerSample > 16))
        {
            return false;
        }
        const quint64 minimumStride = static_cast<quint64>(header.width) * bytesPerPixel;
        if (minimumStride > static_cast<quint64>((std::numeric_limits<quint32>::max)())) return false;
        if (header.width > static_cast<quint32>((std::numeric_limits<int>::max)())) return false;
        if (header.height > static_cast<quint32>((std::numeric_limits<int>::max)())) return false;
        if (header.stride > static_cast<quint32>((std::numeric_limits<int>::max)())) return false;
        if (header.stride < minimumStride) return false;
        if ((header.stride % bytesPerPixel) != 0) return false;

        const quint64 rawSize = sharedFramePayloadSize(header);
        if (rawSize == 0 || rawSize > static_cast<quint64>(kSharedFrameMaxBytes)) return false;

        return true;
    }

    // Claims one ready ring slot so the producer cannot overwrite it while copying
    static bool claimFrameSlot(uchar* slotPtr, SharedFrameHeader& header)
    {
        auto& stateValue = *reinterpret_cast<quint32*>(slotPtr);
        std::atomic_ref<quint32> state(stateValue);
        quint32 expected = 2;
        if (!state.compare_exchange_strong(expected,
                                           3,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire))
        {
            return false;
        }

        memcpy(&header, slotPtr, sizeof(header));
        header.state = 2;
        if (headerLooksSane(header))
        {
            return true;
        }

        state.store(2, std::memory_order_release);
        return false;
    }

    // Releases one claimed ring slot back to the producer
    static void releaseFrameSlot(uchar* slotPtr)
    {
        auto& stateValue = *reinterpret_cast<quint32*>(slotPtr);
        std::atomic_ref<quint32>(stateValue).store(2, std::memory_order_release);
    }

    AgentCameraBackend::AgentCameraBackend()
    {
        m_frameThread.setObjectName(QStringLiteral("ScopeOneAgentFrameReader"));
        m_frameWorker = new AgentFrameWorker(this);
        m_frameWorker->moveToThread(&m_frameThread);
        QObject::connect(&m_frameThread, &QThread::finished,
                         m_frameWorker, &QObject::deleteLater);
        m_frameThread.start();
    }

    AgentCameraBackend::~AgentCameraBackend()
    {
        shutdown();
        m_frameThread.quit();
        m_frameThread.wait();
        m_frameWorker = nullptr;
    }

    void AgentCameraBackend::shutdown()
    {
        if (m_shuttingDown)
        {
            return;
        }
        m_shuttingDown = true;
        CameraBackend::setRecordingFrameDeliveryEnabled(false);

        const bool hadRunningCamera = hasRunningCamera();
        for (auto& slot : m_cameras)
        {
            slot->isRunning = false;
        }
        if (m_frameWorker && m_frameThread.isRunning())
        {
            AgentFrameWorker* const worker = m_frameWorker;
            QMetaObject::invokeMethod(worker,
                                      [worker]() { worker->clear(); },
                                      Qt::BlockingQueuedConnection);
        }
        for (auto& slot : m_cameras)
        {
            if (slot->control)
            {
                QJsonObject request;
                request.insert(agent::kMessageTypeField, agent::kCommandShutdown);
                sendControlCommand(slot->cameraId, request, nullptr, 800);
                slot->control->stop();
            }
            if (slot->process)
            {
                slot->process->terminate();
                if (!slot->process->waitForFinished(1500))
                {
                    slot->process->kill();
                    slot->process->waitForFinished(1000);
                }
            }
        }
        m_cameras.clear();
        discardPendingPreviewFrames();
        if (hadRunningCamera)
        {
            notifyPreviewStopped();
        }
    }

    void AgentFrameWorker::addCamera(const QString& cameraId, const QString& shmKey)
    {
        removeCamera(cameraId);
        auto slot = std::make_shared<ReaderSlot>();
        slot->cameraId = cameraId;
        slot->shmKey = shmKey;
        slot->shm = std::make_unique<QSharedMemory>();
        slot->shm->setNativeKey(shmKey);
        m_readers.insert(cameraId, std::move(slot));
    }

    void AgentFrameWorker::removeCamera(const QString& cameraId)
    {
        m_readers.remove(cameraId);
    }

    void AgentFrameWorker::clear()
    {
        m_readers.clear();
    }

    bool AgentFrameWorker::ensureSharedMemory(const QString& cameraId)
    {
        const auto it = m_readers.find(cameraId);
        return it != m_readers.end() && ensureSharedMemory(*it.value());
    }

    bool AgentFrameWorker::ensureSharedMemory(ReaderSlot& slot)
    {
        const int expectedSize =
            kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
        if (slot.shm->isAttached())
        {
            if (slot.shm->size() >= expectedSize)
            {
                return true;
            }
            qWarning().noquote()
                << QString("SHM size mismatch for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
            slot.shm->detach();
            return false;
        }
        if (!slot.shm->attach(QSharedMemory::ReadWrite))
        {
            qWarning().noquote()
                << QString("SHM attach failed for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
            return false;
        }
        if (slot.shm->size() < expectedSize)
        {
            qWarning().noquote()
                << QString("SHM layout mismatch for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
            slot.shm->detach();
            return false;
        }
        qInfo().noquote()
            << QString("SHM attached for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
        return true;
    }

    // Waits for one agent control channel to become ready
    bool AgentCameraBackend::waitForControlReady(AgentCameraSlot& slot, int timeoutMs)
    {
        return slot.control && slot.control->waitForReady(timeoutMs);
    }

    bool AgentCameraBackend::addFrameReader(const QString& cameraId, const QString& shmKey)
    {
        if (!m_frameWorker || !m_frameThread.isRunning())
        {
            return false;
        }
        AgentFrameWorker* const worker = m_frameWorker;
        return QMetaObject::invokeMethod(
            worker,
            [worker, cameraId, shmKey]() { worker->addCamera(cameraId, shmKey); },
            Qt::BlockingQueuedConnection);
    }

    void AgentCameraBackend::removeFrameReader(const QString& cameraId)
    {
        if (m_frameWorker && m_frameThread.isRunning())
        {
            AgentFrameWorker* const worker = m_frameWorker;
            QMetaObject::invokeMethod(
                worker,
                [worker, cameraId]() { worker->removeCamera(cameraId); },
                Qt::BlockingQueuedConnection);
        }
    }

    bool AgentCameraBackend::prepareFrameReader(const QString& cameraId)
    {
        if (!m_frameWorker || !m_frameThread.isRunning())
        {
            return false;
        }
        bool attached = false;
        AgentFrameWorker* const worker = m_frameWorker;
        const bool invoked = QMetaObject::invokeMethod(
            worker,
            [worker, cameraId, &attached]() { attached = worker->ensureSharedMemory(cameraId); },
            Qt::BlockingQueuedConnection);
        return invoked && attached;
    }

    // Sends one JSON command to an agent control channel
    bool AgentCameraBackend::sendControlCommand(const QString& cameraId,
                                                const QJsonObject& request,
                                                QJsonObject* response,
                                                int timeoutMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const auto it = m_cameras.constFind(normalizedId);
        if (it == m_cameras.constEnd() || !it.value() || !it.value()->control)
        {
            return false;
        }

        bool completed = false;
        bool ok = false;
        QJsonObject capturedResponse;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        watchdog.setInterval((std::max)(1, timeoutMs + 250));
        connect(&watchdog, &QTimer::timeout, &loop, [&]()
        {
            if (!completed)
            {
                completed = true;
                ok = false;
            }
            loop.quit();
        });

        if (!it.value()->control->sendRequest(
            request,
            (std::max)(1, timeoutMs),
            [&](bool requestOk, const QJsonObject& requestResponse, const QString&)
            {
                if (completed)
                {
                    return;
                }
                completed = true;
                ok = requestOk;
                capturedResponse = requestResponse;
                loop.quit();
            }))
        {
            return false;
        }

        watchdog.start();
        if (!completed)
        {
            loop.exec();
        }

        if (!ok)
        {
            return false;
        }
        if (response)
        {
            *response = capturedResponse;
        }
        return true;
    }

    // Selects the requested producer delivery policy in one agent
    bool AgentCameraBackend::sendFrameDeliveryMode(const QString& cameraId,
                                                   AgentFrameDeliveryMode mode)
    {
        QJsonObject request;
        request.insert(agent::kMessageTypeField, agent::kCommandSetFrameDeliveryMode);
        request.insert(QStringLiteral("mode"), frameDeliveryModeName(mode));
        QJsonObject response;
        return sendControlCommand(cameraId, request, &response, 1200)
            && response.value(QStringLiteral("ok")).toBool(false);
    }

    bool AgentFrameWorker::copyFrame(ReaderSlot& slot,
                                     const SharedFrameHeader& header,
                                     const uchar* pixelData,
                                     QList<ImageFrame>& frames)
    {
        const quint64 rawSize = sharedFramePayloadSize(header);
        QByteArray payload;
        payload.resize(static_cast<qsizetype>(rawSize));
        memcpy(payload.data(), pixelData, static_cast<size_t>(rawSize));

        ImageFrame frame = ImageFrame::fromSharedFrame(slot.cameraId, header, payload);
        if (!frame.isValid())
        {
            return false;
        }
        slot.latestFrame = frame;
        frames.append(std::move(frame));
        return true;
    }

    // Reads only the newest ready frame for responsive preview delivery
    bool AgentFrameWorker::readLatestFrame(ReaderSlot& slot,
                                           QList<ImageFrame>& frames,
                                           quint64& acquiredFrameCount)
    {
        auto* base = static_cast<uchar*>(slot.shm->data());
        if (!base)
        {
            return false;
        }

        const int slotStride = kSharedFrameSlotStride;
        const int baseOffset = kSharedMemoryControlSize;
        auto* control = reinterpret_cast<SharedMemoryControl*>(base);

        const quint64 previousFrameIndex = slot.lastFrameIndex;
        SharedFrameHeader capturedHeader{};
        uchar* capturedSlot = nullptr;

        auto claimCandidate = [&](quint32 idx) -> bool
        {
            if (idx >= kSharedFrameNumSlots) return false;
            uchar* ptr = base + baseOffset + idx * slotStride;
            SharedFrameHeader header{};
            if (!claimFrameSlot(ptr, header)) return false;
            if (header.frameIndex <= slot.lastFrameIndex)
            {
                releaseFrameSlot(ptr);
                return false;
            }
            capturedHeader = header;
            capturedSlot = ptr;
            return true;
        };

        const quint32 latestIdx = std::atomic_ref<quint32>(control->latestSlotIndex)
                                      .load(std::memory_order_acquire);
        claimCandidate(latestIdx);

        if (!capturedSlot)
        {
            quint64 bestIndex = slot.lastFrameIndex;
            for (int i = 0; i < kSharedFrameNumSlots; ++i)
            {
                uchar* ptr = base + baseOffset + i * slotStride;
                SharedFrameHeader header{};
                if (!claimFrameSlot(ptr, header)) continue;
                if (header.frameIndex > bestIndex)
                {
                    if (capturedSlot)
                    {
                        releaseFrameSlot(capturedSlot);
                    }
                    bestIndex = header.frameIndex;
                    capturedHeader = header;
                    capturedSlot = ptr;
                }
                else
                {
                    releaseFrameSlot(ptr);
                }
            }
        }

        const bool ok = capturedSlot
            && copyFrame(slot,
                         capturedHeader,
                         capturedSlot + kSharedFrameHeaderSize,
                         frames);
        if (capturedSlot)
        {
            releaseFrameSlot(capturedSlot);
        }
        if (ok)
        {
            slot.lastFrameIndex = capturedHeader.frameIndex;
            acquiredFrameCount = capturedHeader.frameIndex > previousFrameIndex
                                     ? capturedHeader.frameIndex - previousFrameIndex
                                     : 1;
        }
        return ok;
    }

    // Reads every retained shared memory frame in order for recording delivery
    bool AgentFrameWorker::readAllFrames(ReaderSlot& slot,
                                         QList<ImageFrame>& frames,
                                         quint64& acquiredFrameCount)
    {
        auto* base = static_cast<uchar*>(slot.shm->data());
        if (!base)
        {
            return false;
        }

        const int slotStride = kSharedFrameSlotStride;
        const int baseOffset = kSharedMemoryControlSize;
        const quint64 lastIndex = slot.lastFrameIndex;

        struct ClaimedSlot
        {
            uchar* ptr{nullptr};
            SharedFrameHeader header{};
        };
        std::vector<ClaimedSlot> claimedSlots;
        claimedSlots.reserve(kSharedFrameNumSlots);

        for (int i = 0; i < kSharedFrameNumSlots; ++i)
        {
            uchar* ptr = base + baseOffset + i * slotStride;
            SharedFrameHeader header{};
            if (!claimFrameSlot(ptr, header)) continue;
            if (header.frameIndex > lastIndex)
            {
                claimedSlots.push_back({ptr, header});
            }
            else
            {
                releaseFrameSlot(ptr);
            }
        }

        if (claimedSlots.empty())
        {
            return false;
        }

        std::sort(claimedSlots.begin(), claimedSlots.end(), [](const ClaimedSlot& a, const ClaimedSlot& b)
        {
            return a.header.frameIndex < b.header.frameIndex;
        });

        frames.reserve(static_cast<qsizetype>(claimedSlots.size()));
        quint64 maxIndex = lastIndex;
        for (const ClaimedSlot& claimed : claimedSlots)
        {
            if (copyFrame(slot,
                          claimed.header,
                          claimed.ptr + kSharedFrameHeaderSize,
                          frames))
            {
                maxIndex = claimed.header.frameIndex;
            }
            releaseFrameSlot(claimed.ptr);
        }
        if (frames.isEmpty())
        {
            return false;
        }

        slot.latestFrame = frames.constLast();
        slot.lastFrameIndex = maxIndex;
        acquiredFrameCount = maxIndex > lastIndex
                                 ? maxIndex - lastIndex
                                 : static_cast<quint64>(frames.size());
        return true;
    }

    // Copies frames on the reader thread and forwards only completed images
    ImageFrame AgentFrameWorker::consumeFrames(const QString& cameraId)
    {
        const auto it = m_readers.find(cameraId);
        if (it == m_readers.end())
        {
            return {};
        }

        ReaderSlot& slot = *it.value();
        QList<ImageFrame> frames;
        quint64 acquiredFrameCount = 0;
        if (ensureSharedMemory(slot))
        {
            if (m_owner->recordingFrameDeliveryEnabled()
                || m_owner->highRateFrameDeliveryEnabled())
            {
                readAllFrames(slot, frames, acquiredFrameCount);
            }
            else
            {
                readLatestFrame(slot, frames, acquiredFrameCount);
            }
        }

        if (!frames.isEmpty())
        {
            m_owner->submitFrames(frames, acquiredFrameCount);
        }
        return slot.latestFrame;
    }

    void AgentFrameWorker::consumeFramesAsync(const QString& cameraId)
    {
        if (!m_owner->m_frameDeliveryPaused.load(std::memory_order_relaxed))
        {
            consumeFrames(cameraId);
        }
        AgentCameraBackend* const owner = m_owner;
        QMetaObject::invokeMethod(owner,
                                  [owner, cameraId]() { owner->completeFrameRead(cameraId); },
                                  Qt::QueuedConnection);
    }

    ImageFrame AgentCameraBackend::consumeFrameNow(const QString& cameraId)
    {
        if (!m_frameWorker || !m_frameThread.isRunning())
        {
            return {};
        }

        ImageFrame frame;
        AgentFrameWorker* const worker = m_frameWorker;
        const bool invoked = QMetaObject::invokeMethod(
            worker,
            [worker, cameraId, &frame]() { frame = worker->consumeFrames(cameraId); },
            Qt::BlockingQueuedConnection);
        return invoked ? frame : ImageFrame{};
    }

    void AgentCameraBackend::scheduleFrameRead(const QString& cameraId)
    {
        const auto it = m_cameras.find(cameraId);
        if (it == m_cameras.end() || !it.value() || !m_frameWorker || m_shuttingDown)
        {
            return;
        }
        AgentCameraSlot& slot = *it.value();
        if (!slot.isRunning || m_frameDeliveryPaused.load(std::memory_order_relaxed))
        {
            return;
        }
        if (slot.frameReadQueued)
        {
            slot.frameReadRequested = true;
            return;
        }

        slot.frameReadQueued = true;
        AgentFrameWorker* const worker = m_frameWorker;
        if (!QMetaObject::invokeMethod(
                worker,
                [worker, cameraId]() { worker->consumeFramesAsync(cameraId); },
                Qt::QueuedConnection))
        {
            slot.frameReadQueued = false;
        }
    }

    void AgentCameraBackend::completeFrameRead(const QString& cameraId)
    {
        const auto it = m_cameras.find(cameraId);
        if (it == m_cameras.end() || !it.value())
        {
            return;
        }
        AgentCameraSlot& slot = *it.value();
        slot.frameReadQueued = false;
        if (!slot.frameReadRequested)
        {
            return;
        }

        slot.frameReadRequested = false;
        scheduleFrameRead(cameraId);
    }

    // Triggers one camera and waits for its event frame
    bool AgentCameraBackend::captureEventFrame(const QString& cameraId,
                                               ImageFrame& frame,
                                               int timeoutMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const auto cameraIt = m_cameras.constFind(normalizedId);
        if (cameraIt == m_cameras.constEnd() || !cameraIt.value()
            || !prepareFrameReader(normalizedId))
        {
            return false;
        }

        const ImageFrame previousFrame = consumeFrameNow(normalizedId);
        const quint64 previousFrameIndex = previousFrame.isValid()
                                               ? previousFrame.frameIndex
                                               : 0;

        QJsonObject req;
        req.insert(agent::kMessageTypeField, agent::kCommandCaptureEvent);
        QJsonObject resp;
        const int waitMs = (timeoutMs > 0) ? timeoutMs : 1500;
        if (!sendControlCommand(normalizedId, req, &resp, waitMs + 1000))
        {
            return false;
        }
        if (!resp.value("ok").toBool(false))
        {
            return false;
        }

        const quint64 targetFrameIndex =
            agent::decodeUInt64(resp.value(QStringLiteral("frameIndex")));
        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() <= waitMs)
        {
            const ImageFrame candidate = consumeFrameNow(normalizedId);
            if (candidate.isValid()
                && candidate.frameIndex > previousFrameIndex
                && (targetFrameIndex == 0 || candidate.frameIndex >= targetFrameIndex))
            {
                frame = candidate;
                return true;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }

        return false;
    }

    // Pauses or resumes polling while another workflow controls capture
    void AgentCameraBackend::setFrameDeliveryPaused(bool paused)
    {
        if (m_frameDeliveryPaused.exchange(paused, std::memory_order_relaxed) == paused)
        {
            return;
        }
        if (!paused)
        {
            for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
            {
                if (it.value() && it.value()->isRunning)
                {
                    scheduleFrameRead(it.key());
                }
            }
        }
    }

    // Switches agent producers before changing the local recording consumer mode
    bool AgentCameraBackend::setRecordingFrameDeliveryEnabled(bool enabled)
    {
        if (enabled && recordingFrameDeliveryEnabled())
        {
            return true;
        }

        const AgentFrameDeliveryMode previewMode =
            nonRecordingDeliveryMode(highRateFrameDeliveryEnabled());
        if (!enabled)
        {
            CameraBackend::setRecordingFrameDeliveryEnabled(false);
            bool ok = true;
            for (auto it = m_cameras.constBegin(); it != m_cameras.constEnd(); ++it)
            {
                ok = it.value() && sendFrameDeliveryMode(it.key(), previewMode) && ok;
            }
            return ok;
        }

        QStringList updatedCameraIds;
        for (auto it = m_cameras.constBegin(); it != m_cameras.constEnd(); ++it)
        {
            if (!it.value()
                || !sendFrameDeliveryMode(it.key(), AgentFrameDeliveryMode::AllFrames))
            {
                for (const QString& cameraId : updatedCameraIds)
                {
                    sendFrameDeliveryMode(cameraId, previewMode);
                }
                return false;
            }
            updatedCameraIds.append(it.key());
        }

        for (const QString& cameraId : updatedCameraIds)
        {
            if (!prepareFrameReader(cameraId))
            {
                for (const QString& updatedCameraId : updatedCameraIds)
                {
                    sendFrameDeliveryMode(updatedCameraId, previewMode);
                }
                return false;
            }
            consumeFrameNow(cameraId);
        }
        return CameraBackend::setRecordingFrameDeliveryEnabled(true);
    }

    // Switches preview producers between display-rate and processing-rate delivery
    bool AgentCameraBackend::setHighRateFrameDeliveryEnabled(bool enabled)
    {
        if (highRateFrameDeliveryEnabled() == enabled)
        {
            return true;
        }

        if (recordingFrameDeliveryEnabled())
        {
            return CameraBackend::setHighRateFrameDeliveryEnabled(enabled);
        }

        const AgentFrameDeliveryMode targetMode = nonRecordingDeliveryMode(enabled);
        const AgentFrameDeliveryMode rollbackMode =
            nonRecordingDeliveryMode(highRateFrameDeliveryEnabled());
        QStringList updatedCameraIds;
        for (auto it = m_cameras.constBegin(); it != m_cameras.constEnd(); ++it)
        {
            if (!it.value() || !sendFrameDeliveryMode(it.key(), targetMode))
            {
                for (const QString& cameraId : updatedCameraIds)
                {
                    sendFrameDeliveryMode(cameraId, rollbackMode);
                }
                return false;
            }
            updatedCameraIds.append(it.key());
        }
        return CameraBackend::setHighRateFrameDeliveryEnabled(enabled);
    }

    // Starts one camera agent process and connects its control channel
    bool AgentCameraBackend::addAgentCamera(const QString& cameraId,
                                            const QString& adapter,
                                            const QString& device,
                                            const QStringList& preInitProperties,
                                            const QStringList& properties,
                                            double exposureMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString adapterName = adapter.trimmed();
        const QString deviceName = device.trimmed();
        if (normalizedId.isEmpty() || adapterName.isEmpty() || deviceName.isEmpty())
        {
            return false;
        }
        if (m_cameras.contains(normalizedId))
        {
            return true;
        }
        auto slot = std::make_shared<AgentCameraSlot>();
        slot->cameraId = normalizedId;
        slot->shmKey = agent::sharedMemoryKey(normalizedId);
        slot->process = std::make_shared<QProcess>();
        slot->control = std::make_shared<AgentControlSession>(normalizedId,
                                                              agent::controlServerName(normalizedId));
        slot->exposureMs = exposureMs;
        const QString agentPath =
            QDir(QCoreApplication::applicationDirPath()).filePath(agent::kExecutableFileName);
        if (!QFileInfo::exists(agentPath))
        {
            qWarning().noquote() << QString("Agent executable not found: %1").arg(agentPath);
            return false;
        }
        QStringList args;
        args << "--cameraId" << normalizedId
            << "--adapter" << adapterName
            << "--device" << deviceName
            << "--shm" << slot->shmKey;
        if (exposureMs > 0.0)
        {
            args << "--exposure" << QString::number(exposureMs, 'f', 4);
        }
        for (const QString& encodedProperty : preInitProperties)
        {
            if (!encodedProperty.isEmpty())
            {
                args << "--preinit" << encodedProperty;
            }
        }
        for (const QString& encodedProperty : properties)
        {
            if (!encodedProperty.isEmpty())
            {
                args << "--property" << encodedProperty;
            }
        }
        slot->process->setProgram(agentPath);
        slot->process->setArguments(args);
        slot->process->setProcessChannelMode(QProcess::MergedChannels);

        QProcess* const process = slot->process.get();
        connect(process, &QProcess::readyReadStandardOutput, this, [normalizedId, process]()
        {
            const QByteArray output = process->readAllStandardOutput();
            if (!output.isEmpty())
            {
                QStringList lines = QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts);
                for (const QString& line : lines)
                {
                    const QString trimmed = line.trimmed();
                    qInfo().noquote() << QString("[Agent %1] %2").arg(normalizedId, trimmed);
                }
            }
        });
        connect(process, &QProcess::finished, this,
                [this, normalizedId](int, QProcess::ExitStatus)
                {
                    const auto it = m_cameras.find(normalizedId);
                    if (it == m_cameras.end() || !it.value())
                    {
                        return;
                    }
                    const bool wasRunning = it.value()->isRunning;
                    it.value()->isRunning = false;
                    const bool wasRecording = recordingFrameDeliveryEnabled();
                    if (wasRecording)
                    {
                        CameraBackend::setRecordingFrameDeliveryEnabled(false);
                    }
                    if (wasRunning)
                    {
                        notifyPreviewStopped();
                    }
                    if (wasRecording)
                    {
                        emit frameDeliveryFailed(
                            QStringLiteral("Camera agent exited for '%1'").arg(normalizedId));
                    }
                });

        if (slot->control)
        {
            slot->control->addReadyHandler([this, normalizedId](bool ready)
            {
                if (ready)
                {
                    emit agentControlServerListening(normalizedId, agent::controlServerName(normalizedId));
                }
            });
            slot->control->addEventHandler([this, normalizedId](const QJsonObject& event)
            {
                const auto it = m_cameras.find(normalizedId);
                if (it == m_cameras.end() || !it.value())
                {
                    return;
                }
                AgentCameraSlot& slot = *it.value();
                const QString type = event.value(agent::kMessageTypeField).toString();
                if (type == agent::kEventFrameAvailable)
                {
                    scheduleFrameRead(normalizedId);
                    return;
                }
                if (type == agent::kEventPreviewState)
                {
                    const bool wasRunning = slot.isRunning;
                    slot.isRunning = event.value(QStringLiteral("running")).toBool(slot.isRunning);
                    if (wasRunning && !slot.isRunning)
                    {
                        notifyPreviewStopped();
                    }
                    return;
                }
                if (type == agent::kEventAgentError)
                {
                    const QString error = QStringLiteral("Agent '%1' error: %2")
                                              .arg(slot.cameraId,
                                                   event.value(QStringLiteral("error")).toString());
                    qWarning().noquote() << error;
                    if (recordingFrameDeliveryEnabled())
                    {
                        CameraBackend::setRecordingFrameDeliveryEnabled(false);
                        emit frameDeliveryFailed(error);
                    }
                    return;
                }
            });
        }

        slot->process->start();
        if (!slot->process->waitForStarted(3000))
        {
            qWarning().noquote() << QString("Failed to start agent for %1").arg(normalizedId);
            return false;
        }
        m_cameras.insert(normalizedId, slot);
        if (slot->control)
        {
            slot->control->start();
        }
        if (!waitForControlReady(*slot, kAgentControlReadyTimeoutMs))
        {
            qWarning().noquote()
                << QString("Agent control session did not become ready for %1 within %2 ms")
                   .arg(normalizedId)
                   .arg(kAgentControlReadyTimeoutMs);
            removeAgentCamera(normalizedId);
            return false;
        }
        if (!addFrameReader(normalizedId, slot->shmKey))
        {
            removeAgentCamera(normalizedId);
            return false;
        }
        const AgentFrameDeliveryMode deliveryMode = recordingFrameDeliveryEnabled()
                                                        ? AgentFrameDeliveryMode::AllFrames
                                                        : nonRecordingDeliveryMode(
                                                              highRateFrameDeliveryEnabled());
        if (!sendFrameDeliveryMode(normalizedId, deliveryMode))
        {
            removeAgentCamera(normalizedId);
            return false;
        }
        return true;
    }

    // Stops one camera agent process and releases its resources
    void AgentCameraBackend::removeAgentCamera(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (!m_cameras.contains(normalizedId))
        {
            return;
        }
        const auto slot = m_cameras.value(normalizedId);
        const bool wasRunning = slot->isRunning;
        slot->isRunning = false;
        removeFrameReader(normalizedId);
        if (slot->control)
        {
            QJsonObject request;
            request.insert(agent::kMessageTypeField, agent::kCommandShutdown);
            sendControlCommand(normalizedId, request, nullptr, 800);
        }
        m_cameras.remove(normalizedId);
        if (slot->control)
        {
            slot->control->stop();
        }
        if (slot->process)
        {
            slot->process->terminate();
            if (!slot->process->waitForFinished(1500))
            {
                slot->process->kill();
                slot->process->waitForFinished(1000);
            }
        }
        if (wasRunning)
        {
            notifyPreviewStopped();
        }
    }

    std::unique_ptr<CameraBackend> createAgentCameraBackend()
    {
        return std::make_unique<AgentCameraBackend>();
    }
}
