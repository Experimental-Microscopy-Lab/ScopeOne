#include "internal/MultiProcessCameraManager.h"
#include "internal/AgentProtocol.h"
#include "scopeone/SharedFrame.h"
#include "MMCore.h"

#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
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

        // Normalizes camera ids before they become manager keys
        QString normalizedCameraId(const QString& cameraId)
        {
            return cameraId.trimmed();
        }

        struct PropertyReadback
        {
            QString value;
            QString type{QStringLiteral("Unknown")};
            bool readOnly{true};
            QStringList allowedValues;
            bool hasLimits{false};
            double lowerLimit{0.0};
            double upperLimit{0.0};
        };
    } // namespace

    struct MultiProcessCameraManager::ControlSession : QObject
    {
        struct PendingRequest
        {
            QByteArray encoded;
            QTimer* timer{nullptr};
            std::function<void(bool, const QJsonObject&, const QString&)> completion;
        };

        explicit ControlSession(const QString& cameraId,
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

    // Route camera work through one backend
    struct MultiProcessCameraManager::CameraBackend
    {
        explicit CameraBackend(MultiProcessCameraManager& ownerRef)
            : owner(ownerRef)
        {
        }

        virtual ~CameraBackend() = default;

        virtual bool isNative() const = 0;
        virtual void shutdown() = 0;
        virtual bool startPreview() = 0;
        virtual bool stopPreview() = 0;
        virtual bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const = 0;
        virtual bool startPreviewFor(const QString& cameraId) = 0;
        virtual bool stopPreviewFor(const QString& cameraId) = 0;
        virtual bool setExposure(const QString& cameraIdOrAll, double exposureMs) = 0;
        virtual QStringList listProperties(const QString& cameraId) = 0;
        virtual bool readPropertyDetails(const QString& cameraId, const QString& name, PropertyReadback& readback) = 0;
        virtual bool setProperty(const QString& cameraId,
                                 const QString& name,
                                 const QString& value,
                                 QString* errorMessage) = 0;
        virtual bool setROI(const QString& cameraId, int x, int y, int width, int height) = 0;
        virtual bool clearROI(const QString& cameraId) = 0;
        virtual bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) = 0;

    protected:
        void scheduleInitialPolls()
        {
            QTimer::singleShot(10, &owner, [&owner = owner]() { owner.pollSharedMemory(); });
            QTimer::singleShot(40, &owner, [&owner = owner]() { owner.pollSharedMemory(); });
            QTimer::singleShot(100, &owner, [&owner = owner]() { owner.pollSharedMemory(); });
        }

        void onPreviewStarted(const QString& cameraId)
        {
            if (isNative() && !owner.m_pollingPaused && !owner.m_pollTimer.isActive())
            {
                owner.m_pollTimer.start();
            }
            if (isNative() && !owner.m_pollingPaused)
            {
                owner.updatePollingInterval();
            }
            emit owner.previewStateChanged(true);
            qInfo().noquote() << QString("Preview started for camera '%1'").arg(cameraId);
            if (isNative())
            {
                scheduleInitialPolls();
            }
        }

        void onPreviewStopped()
        {
            const bool allStopped = !owner.hasRunningCamera();

            if (allStopped && owner.m_pollTimer.isActive())
            {
                owner.m_pollTimer.stop();
            }
            if (allStopped)
            {
                emit owner.previewStateChanged(false);
                qInfo().noquote() << "All cameras stopped";
            }
            else if (isNative() && !owner.m_pollingPaused)
            {
                owner.updatePollingInterval();
            }
        }

        MultiProcessCameraManager& owner;
    };

    struct MultiProcessCameraManager::DeviceControlBackend : CameraBackend
    {
        using CameraBackend::CameraBackend;

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const override
        {
            QString cameraId;
            if (!resolvePrimaryCameraId(cameraIdOrAll, cameraId))
            {
                return false;
            }
            return readExposureFor(cameraId, exposureMs);
        }

        bool setExposure(const QString& cameraIdOrAll, double exposureMs) override
        {
            const QStringList cameraIds = resolveTargetCameraIds(cameraIdOrAll);
            if (cameraIds.isEmpty())
            {
                return false;
            }

            for (const QString& cameraId : cameraIds)
            {
                if (!applyWithPreviewRestart(cameraId, [&]()
                {
                    if (!writeExposureFor(cameraId, exposureMs))
                    {
                        return false;
                    }
                    double actualExposureMs = exposureMs;
                    readExposureFor(cameraId, actualExposureMs);
                    owner.m_cameras[cameraId]->exposureMs = actualExposureMs;
                    return true;
                }))
                {
                    return false;
                }
            }

            owner.updatePollingInterval();
            return true;
        }

        QStringList listProperties(const QString& cameraId) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return {};
            }
            return listPropertiesFor(resolvedCameraId);
        }

        bool readPropertyDetails(const QString& cameraId, const QString& name, PropertyReadback& readback) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return false;
            }
            return readPropertyDetailsFor(resolvedCameraId, name, readback);
        }

        bool setProperty(const QString& cameraId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return false;
            }
            return applyWithPreviewRestart(
                resolvedCameraId, [&]() { return setPropertyFor(resolvedCameraId, name, value, errorMessage); });
        }

        bool setROI(const QString& cameraId, int x, int y, int width, int height) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return false;
            }
            return applyWithPreviewRestart(
                resolvedCameraId, [&]() { return setROIFor(resolvedCameraId, x, y, width, height); });
        }

        bool clearROI(const QString& cameraId) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return false;
            }
            return applyWithPreviewRestart(resolvedCameraId, [&]() { return clearROIFor(resolvedCameraId); });
        }

        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) override
        {
            QString resolvedCameraId;
            if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
            {
                return false;
            }
            return getROIFor(resolvedCameraId, x, y, width, height);
        }

    protected:
        template <typename Operation>
        bool applyWithPreviewRestart(const QString& cameraId, Operation&& operation)
        {
            // Restart preview around camera changes
            const bool wasRunning = owner.m_cameras.contains(cameraId) && owner.m_cameras.value(cameraId)->isRunning;
            if (wasRunning && !stopPreviewFor(cameraId))
            {
                return false;
            }

            const bool ok = operation();

            if (wasRunning && !startPreviewFor(cameraId))
            {
                return false;
            }

            return ok;
        }

        virtual bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const = 0;
        virtual QStringList resolveTargetCameraIds(const QString& cameraIdOrAll) const = 0;
        virtual bool readExposureFor(const QString& cameraId, double& exposureMs) const = 0;
        virtual bool writeExposureFor(const QString& cameraId, double exposureMs) = 0;
        virtual QStringList listPropertiesFor(const QString& cameraId) = 0;
        virtual bool readPropertyDetailsFor(const QString& cameraId,
                                            const QString& name,
                                            PropertyReadback& readback) = 0;
        virtual bool setPropertyFor(const QString& cameraId,
                                    const QString& name,
                                    const QString& value,
                                    QString* errorMessage) = 0;
        virtual bool setROIFor(const QString& cameraId, int x, int y, int width, int height) = 0;
        virtual bool clearROIFor(const QString& cameraId) = 0;
        virtual bool getROIFor(const QString& cameraId, int& x, int& y, int& width, int& height) = 0;
    };

    struct MultiProcessCameraManager::NativeSingleCameraBackend final : DeviceControlBackend
    {
    public:
        explicit NativeSingleCameraBackend(MultiProcessCameraManager& owner)
            : DeviceControlBackend(owner)
        {
        }

        bool isNative() const override { return true; }

        void shutdown() override
        {
            stopPreview();
            owner.m_cameras.clear();
            owner.m_singleCameraId.clear();
        }

        bool startPreview() override
        {
            return startPreviewFor(owner.m_singleCameraId);
        }

        bool stopPreview() override
        {
            return stopPreviewFor(owner.m_singleCameraId);
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }
            if (!owner.m_cameras.contains(cameraId))
            {
                return false;
            }

            auto slotPtr = owner.m_cameras.value(cameraId);
            MultiProcessCameraManager::CameraSlot& slot = *slotPtr;
            try
            {
                owner.m_nativeCore->setCameraDevice(cameraId.toStdString().c_str());
                while (owner.m_nativeCore->getRemainingImageCount() > 0)
                {
                    owner.m_nativeCore->popNextImage();
                }
                if (!owner.m_nativeCore->isSequenceRunning())
                {
                    owner.m_nativeCore->startContinuousSequenceAcquisition(0.0);
                }
                try
                {
                    const double exposureMs = owner.m_nativeCore->getExposure();
                    if (exposureMs > 0.0)
                    {
                        slot.exposureMs = exposureMs;
                    }
                }
                catch (const CMMError&)
                {
                }
            }
            catch (const CMMError&)
            {
                qWarning().noquote() << QString("Failed to start local preview for camera '%1'").arg(cameraId);
                return false;
            }

            slot.isRunning = true;
            onPreviewStarted(cameraId);
            return true;
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }

            try
            {
                if (owner.m_nativeCore->isSequenceRunning())
                {
                    owner.m_nativeCore->stopSequenceAcquisition();
                }
            }
            catch (const CMMError&)
            {
                return false;
            }

            if (owner.m_cameras.contains(cameraId))
            {
                owner.m_cameras[cameraId]->isRunning = false;
            }
            onPreviewStopped();
            return true;
        }

    protected:
        bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const override
        {
            const QString target =
                cameraIdOrAll.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0
                    ? owner.m_singleCameraId
                    : cameraIdOrAll;
            if (!owner.isSingleCamera(target))
            {
                return false;
            }
            cameraId = target;
            return true;
        }

        QStringList resolveTargetCameraIds(const QString& cameraIdOrAll) const override
        {
            QString cameraId;
            return resolvePrimaryCameraId(cameraIdOrAll, cameraId) ? QStringList{cameraId} : QStringList{};
        }

        bool readExposureFor(const QString& cameraId, double& exposureMs) const override
        {
            if (!owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                exposureMs = owner.m_nativeCore->getExposure(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError&)
            {
                if (owner.m_cameras.contains(cameraId))
                {
                    exposureMs = owner.m_cameras.value(cameraId)->exposureMs;
                    return exposureMs > 0.0;
                }
                return false;
            }
        }

        bool writeExposureFor(const QString& cameraId, double exposureMs) override
        {
            if (!owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                owner.m_nativeCore->setCameraDevice(cameraId.toStdString().c_str());
                owner.m_nativeCore->setExposure(exposureMs);
                owner.m_nativeCore->waitForDevice(cameraId.toStdString().c_str());
                while (owner.m_nativeCore->getRemainingImageCount() > 0)
                {
                    owner.m_nativeCore->popNextImage();
                }
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        }

        QStringList listPropertiesFor(const QString& cameraId) override
        {
            QStringList out;
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return out;
            }
            try
            {
                const auto names = owner.m_nativeCore->getDevicePropertyNames(cameraId.toStdString().c_str());
                for (const auto& name : names)
                {
                    out << QString::fromStdString(name);
                }
            }
            catch (const CMMError&)
            {
                return {};
            }
            return out;
        }

        bool readPropertyDetailsFor(const QString& cameraId, const QString& name, PropertyReadback& readback) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }

            try
            {
                readback.value = QString::fromStdString(
                    owner.m_nativeCore->getProperty(cameraId.toStdString().c_str(), name.toStdString().c_str()));
                try
                {
                    const MM::PropertyType type =
                        owner.m_nativeCore->getPropertyType(cameraId.toStdString().c_str(), name.toStdString().c_str());
                    switch (type)
                    {
                    case MM::String: readback.type = QStringLiteral("String");
                        break;
                    case MM::Float: readback.type = QStringLiteral("Float");
                        break;
                    case MM::Integer: readback.type = QStringLiteral("Integer");
                        break;
                    default: readback.type = QStringLiteral("Unknown");
                        break;
                    }
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    readback.readOnly =
                        owner.m_nativeCore->isPropertyReadOnly(cameraId.toStdString().c_str(),
                                                               name.toStdString().c_str());
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    const auto allowed = owner.m_nativeCore->getAllowedPropertyValues(cameraId.toStdString().c_str(),
                                                                                     name.toStdString().c_str());
                    for (const auto& value : allowed)
                    {
                        readback.allowedValues.append(QString::fromStdString(value));
                    }
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    readback.hasLimits =
                        owner.m_nativeCore->hasPropertyLimits(cameraId.toStdString().c_str(),
                                                              name.toStdString().c_str());
                    if (readback.hasLimits)
                    {
                        readback.lowerLimit = owner.m_nativeCore->getPropertyLowerLimit(cameraId.toStdString().c_str(),
                            name.toStdString().c_str());
                        readback.upperLimit = owner.m_nativeCore->getPropertyUpperLimit(cameraId.toStdString().c_str(),
                            name.toStdString().c_str());
                    }
                }
                catch (const CMMError&)
                {
                }
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        }

        bool setPropertyFor(const QString& cameraId,
                            const QString& name,
                            const QString& value,
                            QString* errorMessage) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                owner.m_nativeCore->setCameraDevice(cameraId.toStdString().c_str());
                owner.m_nativeCore->setProperty(cameraId.toStdString().c_str(),
                                                name.toStdString().c_str(),
                                                value.toStdString().c_str());
                owner.m_nativeCore->waitForDevice(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError& e)
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromStdString(e.getMsg());
                }
                return false;
            }
        }

        bool setROIFor(const QString& cameraId, int x, int y, int width, int height) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                owner.m_nativeCore->setROI(cameraId.toStdString().c_str(), x, y, width, height);
                owner.m_nativeCore->waitForDevice(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError& e)
            {
                qWarning().noquote() << QString("Failed to set ROI for '%1': %2").arg(cameraId, e.getMsg().c_str());
                return false;
            }
        }

        bool clearROIFor(const QString& cameraId) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                owner.m_nativeCore->setCameraDevice(cameraId.toStdString().c_str());
                owner.m_nativeCore->clearROI();
                owner.m_nativeCore->waitForDevice(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError& e)
            {
                qWarning().noquote() << QString("Failed to clear ROI for '%1': %2").arg(cameraId, e.getMsg().c_str());
                return false;
            }
        }

        bool getROIFor(const QString& cameraId, int& x, int& y, int& width, int& height) override
        {
            if (!owner.isSingleCamera(cameraId) || !owner.m_nativeCore)
            {
                return false;
            }
            try
            {
                owner.m_nativeCore->getROI(cameraId.toStdString().c_str(), x, y, width, height);
                return true;
            }
            catch (const CMMError& e)
            {
                qWarning().noquote() << QString("Failed to get ROI for '%1': %2").arg(cameraId, e.getMsg().c_str());
                return false;
            }
        }
    };

    struct MultiProcessCameraManager::AgentCameraBackend final : DeviceControlBackend
    {
    public:
        explicit AgentCameraBackend(MultiProcessCameraManager& owner)
            : DeviceControlBackend(owner)
        {
        }

        bool isNative() const override { return false; }

        void shutdown() override
        {
            const bool hadRunningCamera = owner.hasRunningCamera();
            owner.m_pollTimer.stop();
            for (auto& ptr : owner.m_cameras)
            {
                if (ptr->control)
                {
                    QJsonObject request;
                    request.insert(agent::kMessageTypeField, agent::kCommandShutdown);
                    owner.sendControlCommand(ptr->cameraId, request, nullptr, 800);
                    ptr->control->stop();
                }
                if (ptr->process)
                {
                    ptr->process->terminate();
                    if (!ptr->process->waitForFinished(1500))
                    {
                        ptr->process->kill();
                        ptr->process->waitForFinished(1000);
                    }
                }
                if (ptr->shm && ptr->shm->isAttached())
                {
                    ptr->shm->detach();
                }
            }
            owner.m_cameras.clear();
            if (hadRunningCamera)
            {
                emit owner.previewStateChanged(false);
            }
        }

        bool startPreview() override
        {
            if (owner.m_cameras.isEmpty())
            {
                return false;
            }

            for (auto it = owner.m_cameras.begin(); it != owner.m_cameras.end(); ++it)
            {
                const QString cameraId = it.key();
                if (!startPreviewFor(cameraId))
                {
                    return false;
                }
            }
            qInfo().noquote() << "Multi-process preview started";
            return true;
        }

        bool stopPreview() override
        {
            bool ok = true;
            for (auto it = owner.m_cameras.begin(); it != owner.m_cameras.end(); ++it)
            {
                ok = stopPreviewFor(it.key()) && ok;
            }
            if (ok)
            {
                qInfo().noquote() << "Multi-process preview stopped";
            }
            return ok;
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            if (!owner.m_cameras.contains(cameraId))
            {
                qWarning().noquote() << QString("Camera '%1' not found").arg(cameraId);
                return false;
            }

            MultiProcessCameraManager::CameraSlot& slot = *owner.m_cameras[cameraId];
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandStartPreview);
            QJsonObject resp;
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200))
            {
                qWarning().noquote() << QString("Failed to connect to camera '%1'").arg(cameraId);
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                qWarning().noquote() << QString("Agent refused to start preview for '%1'").arg(cameraId);
                return false;
            }

            int tries = 0;
            while (!owner.ensureSharedMemory(slot) && tries++ < 20)
            {
                QThread::msleep(50);
            }
            if (!slot.shm || !slot.shm->isAttached())
            {
                qWarning().noquote() << QString("Shared memory unavailable for '%1'").arg(cameraId);
                return false;
            }

            slot.isRunning = true;
            onPreviewStarted(cameraId);
            return true;
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandStopPreview);
            QJsonObject resp;
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200))
            {
                return false;
            }
            if (!resp.value(QStringLiteral("ok")).toBool(false))
            {
                return false;
            }

            if (owner.m_cameras.contains(cameraId))
            {
                owner.m_cameras[cameraId]->isRunning = false;
            }
            onPreviewStopped();
            return true;
        }

    protected:
        bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const override
        {
            if (owner.m_cameras.isEmpty())
            {
                return false;
            }

            const QString target =
                cameraIdOrAll.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0
                    ? owner.m_cameras.firstKey()
                    : cameraIdOrAll;
            if (!owner.m_cameras.contains(target))
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
                return owner.m_cameras.keys();
            }
            return owner.m_cameras.contains(cameraIdOrAll) ? QStringList{cameraIdOrAll} : QStringList{};
        }

        bool readExposureFor(const QString& cameraId, double& exposureMs) const override
        {
            const auto it = owner.m_cameras.constFind(cameraId);
            if (it == owner.m_cameras.constEnd() || !it.value())
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
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200)
                || !resp.value(QStringLiteral("ok")).toBool(false))
            {
                return false;
            }
            const double actualExposureMs = resp.value(QStringLiteral("exposureMs")).toDouble(exposureMs);
            owner.m_cameras[cameraId]->exposureMs = actualExposureMs;
            return true;
        }

        QStringList listPropertiesFor(const QString& cameraId) override
        {
            QStringList out;
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandListProperties);
            QJsonObject resp;
            if (!owner.sendControlCommand(cameraId, req, &resp, 4000))
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

        bool readPropertyDetailsFor(const QString& cameraId, const QString& name, PropertyReadback& readback) override
        {
            QJsonObject req;
            req.insert(agent::kMessageTypeField, agent::kCommandGetProperty);
            req.insert(QStringLiteral("name"), name);
            QJsonObject resp;
            if (!owner.sendControlCommand(cameraId, req, &resp, 4000))
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
            if (!owner.sendControlCommand(cameraId, req, &resp, 4000))
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
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200))
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
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200))
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
            if (!owner.sendControlCommand(cameraId, req, &resp, 1200))
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
    };

    // Creates the camera manager and shared memory polling timer
    MultiProcessCameraManager::MultiProcessCameraManager(QObject* parent)
        : QObject(parent)
    {
        m_pollTimer.setInterval(1);
        connect(&m_pollTimer, &QTimer::timeout, this, &MultiProcessCameraManager::pollSharedMemory);
    }

    // Sets the native MMCore instance for single camera mode
    void MultiProcessCameraManager::setNativeCore(const std::shared_ptr<CMMCore>& core)
    {
        m_nativeCore = core;
    }

    // Prepares native single camera preview without launching an agent
    bool MultiProcessCameraManager::startSingleCamera(const QString& cameraId, double exposureMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (!m_nativeCore)
        {
            return false;
        }
        if (normalizedId.isEmpty())
        {
            return false;
        }
        if (m_runtime && !m_runtime->isNative())
        {
            stopAgents();
        }
        else if (m_runtime)
        {
            m_runtime->shutdown();
        }
        if (!m_runtime)
        {
            m_runtime = std::make_unique<NativeSingleCameraBackend>(*this);
        }

        m_singleCameraId = normalizedId;
        clearPropertyCaches();
        m_cameras.clear();
        auto slot = std::make_shared<CameraSlot>();
        slot->cameraId = normalizedId;
        slot->exposureMs = exposureMs;
        slot->isRunning = false;
        m_cameras.insert(normalizedId, slot);

        return true;
    }

    // Returns payload byte count from a shared frame header
    static quint64 sharedFramePayloadSize(const SharedFrameHeader& header)
    {
        return static_cast<quint64>(header.stride) * header.height;
    }

    // Validates one shared frame header before reading pixels
    static bool headerLooksSane(const SharedFrameHeader& header)
    {
        if (header.state > 2) return false;
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

    // Updates native polling interval from active exposure times
    void MultiProcessCameraManager::updatePollingInterval()
    {
        if (m_runtime && !m_runtime->isNative())
        {
            return;
        }

        double minExposureMs = 1000.0;
        bool hasRunningCamera = false;

        for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
        {
            const CameraSlot& slot = *it.value();
            if (slot.isRunning && slot.exposureMs > 0)
            {
                minExposureMs = (std::min)(minExposureMs, slot.exposureMs);
                hasRunningCamera = true;
            }
        }

        if (!hasRunningCamera)
        {
            return;
        }

        const double targetIntervalMs = minExposureMs / 2.0;
        int newInterval = static_cast<int>((std::max)(1.0, (std::min)(500.0, targetIntervalMs)));

        if (m_pollTimer.interval() != newInterval)
        {
            m_pollTimer.setInterval(newInterval);
        }
    }

    // Stops all camera backends during teardown
    MultiProcessCameraManager::~MultiProcessCameraManager()
    {
        stopAgents();
    }

    // Checks whether any configured camera is currently running
    bool MultiProcessCameraManager::hasRunningCamera() const
    {
        for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
        {
            if (it.value() && it.value()->isRunning)
            {
                return true;
            }
        }
        return false;
    }

    // Clears cached property metadata across camera backends
    void MultiProcessCameraManager::clearPropertyCaches()
    {
        m_propertyTypeCache.clear();
        m_propertyReadOnlyCache.clear();
        m_propertyAllowedValuesCache.clear();
        m_propertyHasLimitsCache.clear();
        m_propertyLowerLimitCache.clear();
        m_propertyUpperLimitCache.clear();
    }

    // Waits for one agent control channel to become ready
    bool MultiProcessCameraManager::waitForControlReady(CameraSlot& slot, int timeoutMs)
    {
        return slot.control && slot.control->waitForReady(timeoutMs);
    }

    // Stops all agent or native camera backends
    void MultiProcessCameraManager::stopAgents()
    {
        if (m_runtime)
        {
            m_runtime->shutdown();
        }
        m_pollTimer.stop();
        m_runtime.reset();
        m_singleCameraId.clear();
        clearPropertyCaches();
    }

    // Ensures the shared memory block for one camera is attached
    bool MultiProcessCameraManager::ensureSharedMemory(CameraSlot& slot)
    {
        if (!slot.shm)
        {
            return false;
        }
        const int expectedSize = kSharedMemoryControlSize + kSharedFrameNumSlots * kSharedFrameSlotStride;
        if (slot.shm->isAttached())
        {
            if (slot.shm->size() < expectedSize)
            {
                qWarning().noquote() << QString("SHM size mismatch for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
                slot.shm->detach();
                return false;
            }
            return true;
        }
        if (!slot.shm->attach(QSharedMemory::ReadOnly))
        {
            qWarning().noquote() << QString("SHM attach failed for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
            return false;
        }
        if (slot.shm->size() < expectedSize)
        {
            qWarning().noquote() << QString("SHM layout mismatch for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
            slot.shm->detach();
            return false;
        }
        qInfo().noquote() << QString("SHM attached for %1 (key=%2)").arg(slot.cameraId, slot.shmKey);
        return true;
    }

    // Checks whether a camera is handled by the native backend
    bool MultiProcessCameraManager::isSingleCamera(const QString& cameraId) const
    {
        return m_runtime && m_runtime->isNative() && !m_singleCameraId.isEmpty() && cameraId == m_singleCameraId;
    }

    // Pulls queued frames from the native MMCore circular buffer
    void MultiProcessCameraManager::pollSingleCamera(CameraSlot& slot)
    {
        if (!m_nativeCore || !slot.isRunning)
        {
            return;
        }

        long remaining = 0;
        try
        {
            remaining = m_nativeCore->getRemainingImageCount();
        }
        catch (const CMMError&)
        {
            return;
        }
        if (remaining <= 0)
        {
            return;
        }

        const unsigned w = m_nativeCore->getImageWidth();
        const unsigned h = m_nativeCore->getImageHeight();
        const unsigned bpp = m_nativeCore->getBytesPerPixel();
        if (bpp != 1 && bpp != 2)
        {
            return;
        }
        const qint64 stride = static_cast<qint64>(w) * bpp;
        const qint64 rawSize = stride * h;
        if (w > static_cast<unsigned>((std::numeric_limits<int>::max)())
            || h > static_cast<unsigned>((std::numeric_limits<int>::max)())
            || stride > (std::numeric_limits<int>::max)()
            || rawSize <= 0
            || rawSize > (std::numeric_limits<qsizetype>::max)())
        {
            return;
        }

        int sourceRoiX = 0;
        int sourceRoiY = 0;
        int sourceRoiWidth = static_cast<int>(w);
        int sourceRoiHeight = static_cast<int>(h);
        try
        {
            m_nativeCore->getROI(slot.cameraId.toStdString().c_str(),
                                 sourceRoiX,
                                 sourceRoiY,
                                 sourceRoiWidth,
                                 sourceRoiHeight);
        }
        catch (const CMMError&)
        {
            sourceRoiX = 0;
            sourceRoiY = 0;
            sourceRoiWidth = static_cast<int>(w);
            sourceRoiHeight = static_cast<int>(h);
        }

        quint16 bitDepth = (bpp == 2) ? 16 : 8;
        try
        {
            const int bd = m_nativeCore->getImageBitDepth();
            const auto pixelFormat = bpp == 2 ? scopeone::core::ImagePixelFormat::Mono16
                                              : scopeone::core::ImagePixelFormat::Mono8;
            bitDepth = static_cast<quint16>(ImageFrame::normalizedBitsPerSample(pixelFormat, bd));
        }
        catch (const CMMError&)
        {
        }

        ImageFrame lastFrame;

        while (remaining-- > 0)
        {
            const void* img = m_nativeCore->popNextImage();
            if (!img)
            {
                break;
            }

            QByteArray payload;
            payload.resize(static_cast<qsizetype>(rawSize));
            memcpy(payload.data(), img, static_cast<size_t>(rawSize));

            ImageFrame frame;
            frame.cameraId = slot.cameraId;
            frame.width = static_cast<int>(w);
            frame.height = static_cast<int>(h);
            frame.stride = static_cast<int>(stride);
            frame.pixelFormat = bpp == 2 ? scopeone::core::ImagePixelFormat::Mono16
                                         : scopeone::core::ImagePixelFormat::Mono8;
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(frame.pixelFormat, bitDepth);
            frame.frameIndex = ++slot.lastFrameIndex;
            frame.timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
            frame.sourceRoiX = sourceRoiX;
            frame.sourceRoiY = sourceRoiY;
            frame.sourceRoiWidth = sourceRoiWidth;
            frame.sourceRoiHeight = sourceRoiHeight;
            frame.bytes = std::move(payload);
            if (frame.isValid())
            {
                emit newRawFrameReady(frame);
                lastFrame = std::move(frame);
            }
        }

        if (lastFrame.isValid())
        {
            slot.latestFrame = std::move(lastFrame);
        }
    }

    // Sends one JSON command to an agent control channel
    bool MultiProcessCameraManager::sendControlCommand(const QString& cameraId,
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

    // Reads the latest ready frame from one shared memory block
    bool MultiProcessCameraManager::readLatestFrame(CameraSlot& slot)
    {
        if (!slot.shm || !slot.shm->isAttached())
        {
            return false;
        }
        if (!slot.shm->lock())
        {
            return false;
        }

        uchar* base = static_cast<uchar*>(slot.shm->data());
        if (!base)
        {
            slot.shm->unlock();
            return false;
        }

        const int slotStride = kSharedFrameSlotStride;
        const int baseOffset = kSharedMemoryControlSize;
        const SharedMemoryControl* control = reinterpret_cast<const SharedMemoryControl*>(base);

        auto copySlotToImage = [&](const SharedFrameHeader& capturedHeader, const uchar* pixelData) -> bool
        {
            const quint64 rawSize = sharedFramePayloadSize(capturedHeader);

            QByteArray payload;
            payload.resize(static_cast<qsizetype>(rawSize));
            memcpy(payload.data(), pixelData, static_cast<size_t>(rawSize));
            slot.latestFrame = ImageFrame::fromSharedFrame(slot.cameraId, capturedHeader, payload);
            slot.lastFrameIndex = capturedHeader.frameIndex;
            return slot.latestFrame.isValid();
        };

        bool ok = false;
        SharedFrameHeader capturedHeader{};

        auto trySlotIndex = [&](quint32 idx) -> bool
        {
            if (idx >= kSharedFrameNumSlots) return false;
            const uchar* ptr = base + baseOffset + idx * slotStride;
            SharedFrameHeader header{};
            memcpy(&header, ptr, sizeof(SharedFrameHeader));
            if (!headerLooksSane(header)) return false;
            if (header.state != 2) return false;
            if (header.frameIndex <= slot.lastFrameIndex) return false;
            capturedHeader = header;
            return copySlotToImage(capturedHeader, ptr + kSharedFrameHeaderSize);
        };

        const quint32 latestIdx = control->latestSlotIndex;
        ok = trySlotIndex(latestIdx);

        if (!ok)
        {
            quint64 bestIndex = slot.lastFrameIndex;
            int bestOffset = -1;
            SharedFrameHeader header{};
            for (int i = 0; i < kSharedFrameNumSlots; ++i)
            {
                const uchar* ptr = base + baseOffset + i * slotStride;
                memcpy(&header, ptr, sizeof(SharedFrameHeader));
                if (!headerLooksSane(header)) continue;
                if (header.state == 2 && header.frameIndex > bestIndex)
                {
                    bestIndex = header.frameIndex;
                    bestOffset = i;
                }
            }

            if (bestOffset >= 0)
            {
                const uchar* ptr = base + baseOffset + bestOffset * slotStride;
                memcpy(&capturedHeader, ptr, sizeof(SharedFrameHeader));
                if (headerLooksSane(capturedHeader) && capturedHeader.state == 2 && capturedHeader.frameIndex > slot.
                    lastFrameIndex)
                {
                    ok = copySlotToImage(capturedHeader, ptr + kSharedFrameHeaderSize);
                }
            }
        }

        slot.shm->unlock();

        return ok;
    }

    // Consumes all new ready frames from one agent shared memory block
    bool MultiProcessCameraManager::consumeAgentFrames(CameraSlot& slot, bool emitFrames)
    {
        if (!slot.isRunning)
        {
            return false;
        }
        if (!ensureSharedMemory(slot))
        {
            return false;
        }
        if (!slot.shm || !slot.shm->lock())
        {
            return false;
        }

        uchar* base = static_cast<uchar*>(slot.shm->data());
        if (!base)
        {
            slot.shm->unlock();
            return false;
        }

        struct PendingSlot
        {
            SharedFrameHeader header{};
            int index{-1};
        };

        const int slotStride = kSharedFrameSlotStride;
        const int baseOffset = kSharedMemoryControlSize;
        const quint64 lastIndex = slot.lastFrameIndex;

        std::vector<PendingSlot> pending;
        pending.reserve(kSharedFrameNumSlots);

        for (int i = 0; i < kSharedFrameNumSlots; ++i)
        {
            const uchar* ptr = base + baseOffset + i * slotStride;
            SharedFrameHeader header{};
            memcpy(&header, ptr, sizeof(SharedFrameHeader));
            if (!headerLooksSane(header)) continue;
            if (header.state != 2) continue;
            if (header.frameIndex <= lastIndex) continue;
            pending.push_back(PendingSlot{header, i});
        }

        if (pending.empty())
        {
            slot.shm->unlock();
            return false;
        }

        std::sort(pending.begin(), pending.end(), [](const PendingSlot& a, const PendingSlot& b)
        {
            return a.header.frameIndex < b.header.frameIndex;
        });

        std::vector<ImageFrame> frames;
        frames.reserve(pending.size());
        quint64 maxIndex = lastIndex;

        for (const PendingSlot& item : pending)
        {
            const SharedFrameHeader& header = item.header;
            const uchar* ptr = base + baseOffset + item.index * slotStride;
            const uchar* pixelData = ptr + kSharedFrameHeaderSize;
            const quint64 rawSize = sharedFramePayloadSize(header);
            QByteArray payload;
            payload.resize(static_cast<qsizetype>(rawSize));
            memcpy(payload.data(), pixelData, static_cast<size_t>(rawSize));
            ImageFrame frame = ImageFrame::fromSharedFrame(slot.cameraId, header, payload);
            if (frame.isValid())
            {
                frames.push_back(frame);
            }
            if (header.frameIndex > maxIndex)
            {
                maxIndex = header.frameIndex;
            }
        }

        slot.lastFrameIndex = maxIndex;
        slot.shm->unlock();

        if (frames.empty())
        {
            return false;
        }

        if (emitFrames)
        {
            for (const ImageFrame& frame : frames)
            {
                if (!frame.isValid())
                {
                    continue;
                }
                emit newRawFrameReady(frame);
            }
        }

        slot.latestFrame = frames.back();
        return true;
    }

    // Triggers one camera and waits for its event frame
    bool MultiProcessCameraManager::captureEventFrame(const QString& cameraId,
                                                      ImageFrame& frame,
                                                      int timeoutMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (!m_cameras.contains(normalizedId))
        {
            return false;
        }
        auto slotPtr = m_cameras.value(normalizedId);
        if (!slotPtr)
        {
            return false;
        }

        const quint64 previousFrameIndex = slotPtr->latestFrame.isValid()
                                               ? slotPtr->latestFrame.frameIndex
                                               : slotPtr->lastFrameIndex;

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
        if (!ensureSharedMemory(*slotPtr))
        {
            return false;
        }

        const auto hasTargetFrame = [&]() -> bool
        {
            return slotPtr->latestFrame.isValid()
                && slotPtr->latestFrame.frameIndex > previousFrameIndex
                && (targetFrameIndex == 0 || slotPtr->latestFrame.frameIndex >= targetFrameIndex);
        };

        if (hasTargetFrame())
        {
            frame = slotPtr->latestFrame;
            return true;
        }

        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() <= waitMs)
        {
            if (hasTargetFrame())
            {
                frame = slotPtr->latestFrame;
                return true;
            }
            if (readLatestFrame(*slotPtr) && hasTargetFrame())
            {
                frame = slotPtr->latestFrame;
                return true;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }

        return false;
    }

    // Pauses or resumes polling while another workflow controls capture
    void MultiProcessCameraManager::setPollingPaused(bool paused)
    {
        if (m_pollingPaused == paused)
        {
            return;
        }
        m_pollingPaused = paused;
        if (m_runtime && m_runtime->isNative())
        {
            if (m_pollingPaused)
            {
                if (m_pollTimer.isActive())
                {
                    m_pollTimer.stop();
                }
                return;
            }
            if (hasRunningCamera() && !m_pollTimer.isActive())
            {
                m_pollTimer.start();
                updatePollingInterval();
            }
            return;
        }

        if (!m_pollingPaused)
        {
            for (auto it = m_cameras.begin(); it != m_cameras.end(); ++it)
            {
                if (it.value() && it.value()->isRunning)
                {
                    consumeAgentFrames(*it.value(), true);
                }
            }
        }
    }

    // Polls the native single camera backend for new frames
    void MultiProcessCameraManager::pollSharedMemory()
    {
        if (!m_runtime || !m_runtime->isNative() || m_pollingPaused)
        {
            return;
        }

        if (m_cameras.contains(m_singleCameraId))
        {
            pollSingleCamera(*m_cameras[m_singleCameraId]);
        }
    }

    // Starts one camera agent process and connects its control channel
    bool MultiProcessCameraManager::startAgentFor(const QString& cameraId,
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
        if (m_runtime && m_runtime->isNative())
        {
            stopAgents();
        }
        if (!m_runtime)
        {
            m_runtime = std::make_unique<AgentCameraBackend>(*this);
            m_singleCameraId.clear();
            clearPropertyCaches();
        }
        if (m_cameras.contains(normalizedId))
        {
            return true;
        }
        auto slot = std::make_shared<CameraSlot>();
        slot->cameraId = normalizedId;
        slot->shmKey = agent::sharedMemoryKey(normalizedId);
        slot->shm = std::make_shared<QSharedMemory>();
        slot->shm->setNativeKey(slot->shmKey);
        slot->process = std::make_shared<QProcess>(this);
        slot->control = std::make_shared<ControlSession>(normalizedId,
                                                         agent::controlServerName(normalizedId),
                                                         this);
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

        connect(slot->process.get(), &QProcess::readyReadStandardOutput, this, [normalizedId, slot]()
        {
            QByteArray output = slot->process->readAllStandardOutput();
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
        connect(slot->process.get(), &QProcess::finished, this,
                [this, slot](int, QProcess::ExitStatus)
                {
                    slot->isRunning = false;
                    if (!hasRunningCamera())
                    {
                        emit previewStateChanged(false);
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
            slot->control->addEventHandler([this, slot](const QJsonObject& event)
            {
                const QString type = event.value(agent::kMessageTypeField).toString();
                if (type == agent::kEventFrameAvailable)
                {
                    consumeAgentFrames(*slot, !m_pollingPaused);
                    return;
                }
                if (type == agent::kEventPreviewState)
                {
                    const bool running = event.value(QStringLiteral("running")).toBool(slot->isRunning);
                    slot->isRunning = running;
                    return;
                }
                if (type == agent::kEventAgentError)
                {
                    qWarning().noquote()
                        << QString("Agent '%1' error: %2")
                        .arg(slot->cameraId,
                             event.value(QStringLiteral("error")).toString());
                    return;
                }
                if (type == agent::kEventBufferOverflow)
                {
                    qWarning().noquote()
                        << QString("Agent '%1' reported MMCore buffer overflow (capacity=%2 frames)")
                           .arg(slot->cameraId)
                           .arg(event.value(QStringLiteral("capacityFrames")).toInt(0));
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
            stopAgentFor(normalizedId);
            return false;
        }
        return true;
    }

    // Stops one camera agent process and releases its resources
    bool MultiProcessCameraManager::stopAgentFor(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (m_runtime && m_runtime->isNative())
        {
            return false;
        }
        if (!m_cameras.contains(normalizedId)) return true;
        if (auto existing = m_cameras.value(normalizedId); existing && existing->control)
        {
            QJsonObject request;
            request.insert(agent::kMessageTypeField, agent::kCommandShutdown);
            sendControlCommand(normalizedId, request, nullptr, 800);
        }
        auto slot = m_cameras.take(normalizedId);
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
        if (slot->shm && slot->shm->isAttached()) slot->shm->detach();
        if (m_cameras.isEmpty())
        {
            if (m_pollTimer.isActive())
            {
                m_pollTimer.stop();
            }
            emit previewStateChanged(false);
            m_singleCameraId.clear();
            clearPropertyCaches();
            m_runtime.reset();
        }
        return true;
    }

    // Starts preview on the active camera backend
    void MultiProcessCameraManager::startPreview()
    {
        if (m_runtime)
        {
            m_runtime->startPreview();
        }
    }

    // Stops preview on the active camera backend
    void MultiProcessCameraManager::stopPreview()
    {
        if (m_runtime)
        {
            m_runtime->stopPreview();
        }
    }

    // Sets exposure through the active camera backend
    bool MultiProcessCameraManager::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QString target = cameraIdOrAll.trimmed();
        return !target.isEmpty() && m_runtime && m_runtime->setExposure(target, exposureMs);
    }

    // Reads exposure through the active camera backend
    bool MultiProcessCameraManager::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        exposureMs = 0.0;
        const QString target = cameraIdOrAll.trimmed();
        return !target.isEmpty() && m_runtime && m_runtime->getExposure(target, exposureMs);
    }

    // Lists properties for one camera
    QStringList MultiProcessCameraManager::listProperties(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (normalizedId.isEmpty() || !m_runtime)
        {
            return {};
        }
        return m_runtime->listProperties(normalizedId);
    }

    // Reads a property and updates property metadata caches
    QString MultiProcessCameraManager::getProperty(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        PropertyReadback readback;
        if (normalizedId.isEmpty() || !m_runtime || !m_runtime->readPropertyDetails(normalizedId, name, readback))
        {
            m_propertyTypeCache.remove(propKey);
            m_propertyReadOnlyCache.remove(propKey);
            m_propertyAllowedValuesCache.remove(propKey);
            m_propertyHasLimitsCache.remove(propKey);
            m_propertyLowerLimitCache.remove(propKey);
            m_propertyUpperLimitCache.remove(propKey);
            return {};
        }

        m_propertyTypeCache[propKey] = readback.type;
        m_propertyReadOnlyCache[propKey] = readback.readOnly;
        m_propertyAllowedValuesCache[propKey] = readback.allowedValues;
        m_propertyHasLimitsCache[propKey] = readback.hasLimits;
        if (readback.hasLimits)
        {
            m_propertyLowerLimitCache[propKey] = readback.lowerLimit;
            m_propertyUpperLimitCache[propKey] = readback.upperLimit;
        }
        else
        {
            m_propertyLowerLimitCache.remove(propKey);
            m_propertyUpperLimitCache.remove(propKey);
        }
        return readback.value;
    }

    // Reads cached property type after refreshing if needed
    QString MultiProcessCameraManager::getPropertyType(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyTypeCache.contains(propKey))
        {
            return m_propertyTypeCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyTypeCache.value(propKey, QStringLiteral("Unknown"));
    }

    // Reads cached property mutability after refreshing if needed
    bool MultiProcessCameraManager::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyReadOnlyCache.contains(propKey))
        {
            return m_propertyReadOnlyCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyReadOnlyCache.value(propKey, true);
    }

    // Writes a camera property and invalidates cached metadata
    bool MultiProcessCameraManager::setProperty(const QString& cameraId,
                                                const QString& name,
                                                const QString& value,
                                                QString* errorMessage)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (normalizedId.isEmpty() || !m_runtime)
        {
            return false;
        }
        const bool ok = m_runtime->setProperty(normalizedId, name, value, errorMessage);
        if (ok)
        {
            const QString propKey = QString("%1:%2").arg(normalizedId, name);
            m_propertyTypeCache.remove(propKey);
            m_propertyReadOnlyCache.remove(propKey);
            m_propertyAllowedValuesCache.remove(propKey);
            m_propertyHasLimitsCache.remove(propKey);
            m_propertyLowerLimitCache.remove(propKey);
            m_propertyUpperLimitCache.remove(propKey);
        }
        return ok;
    }

    // Reports whether one camera preview is running
    bool MultiProcessCameraManager::isPreviewRunning(const QString& cameraId) const
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const auto it = m_cameras.constFind(normalizedId);
        return it != m_cameras.constEnd() && it.value() && it.value()->isRunning;
    }

    // Starts preview for one camera
    bool MultiProcessCameraManager::startPreviewFor(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_runtime && m_runtime->startPreviewFor(normalizedId);
    }

    // Stops preview for one camera
    bool MultiProcessCameraManager::stopPreviewFor(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_runtime && m_runtime->stopPreviewFor(normalizedId);
    }

    // Reads cached allowed property values after refreshing if needed
    QStringList MultiProcessCameraManager::getAllowedPropertyValues(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyAllowedValuesCache.contains(propKey))
        {
            return m_propertyAllowedValuesCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyAllowedValuesCache.value(propKey, QStringList());
    }

    // Reads cached property limit availability after refreshing if needed
    bool MultiProcessCameraManager::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyHasLimitsCache.contains(propKey))
        {
            return m_propertyHasLimitsCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyHasLimitsCache.value(propKey, false);
    }

    // Reads cached lower property limit after refreshing if needed
    double MultiProcessCameraManager::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyLowerLimitCache.contains(propKey))
        {
            return m_propertyLowerLimitCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyLowerLimitCache.value(propKey, 0.0);
    }

    // Reads cached upper property limit after refreshing if needed
    double MultiProcessCameraManager::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        QString propKey = QString("%1:%2").arg(normalizedId, name);
        if (m_propertyUpperLimitCache.contains(propKey))
        {
            return m_propertyUpperLimitCache[propKey];
        }
        getProperty(normalizedId, name);
        return m_propertyUpperLimitCache.value(propKey, 0.0);
    }

    // Sets ROI through the active camera backend
    bool MultiProcessCameraManager::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_runtime && m_runtime->setROI(normalizedId, x, y, width, height);
    }

    // Clears ROI through the active camera backend
    bool MultiProcessCameraManager::clearROI(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_runtime && m_runtime->clearROI(normalizedId);
    }

    // Reads ROI through the active camera backend
    bool MultiProcessCameraManager::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_runtime && m_runtime->getROI(normalizedId, x, y, width, height);
    }
} // namespace scopeone::core::internal
