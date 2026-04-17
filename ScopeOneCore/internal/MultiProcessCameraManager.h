#pragma once

#include <QObject>
#include <QByteArray>
#include <QProcess>
#include <QSharedMemory>
#include <QTimer>
#include <QMap>
#include <QString>
#include <QStringList>
#include <memory>

#include "scopeone/SharedFrame.h"

class QJsonObject;
class CMMCore;

namespace scopeone::core::internal
{
    class MultiProcessCameraManager : public QObject
    {
        Q_OBJECT

    public:
        explicit MultiProcessCameraManager(QObject* parent = nullptr);
        ~MultiProcessCameraManager();

        void setNativeCore(const std::shared_ptr<CMMCore>& core);
        bool startSingleCamera(const QString& cameraId, double exposureMs = 0.0);

        void stopAgents();

        bool startAgentFor(const QString& cameraId, const QString& adapter, const QString& device,
                           const QStringList& preInitProperties = QStringList(),
                           const QStringList& properties = QStringList(),
                           double exposureMs = 0.0);
        bool stopAgentFor(const QString& cameraId);

        void startPreview();
        void stopPreview();
        void setPollingPaused(bool paused);

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs);
        bool startPreviewFor(const QString& cameraId);
        bool stopPreviewFor(const QString& cameraId);
        QStringList listProperties(const QString& cameraId);
        QString getProperty(const QString& cameraId, const QString& name);
        bool setProperty(const QString& cameraId, const QString& name, const QString& value);
        QString getPropertyType(const QString& cameraId, const QString& name);
        bool isPropertyReadOnly(const QString& cameraId, const QString& name);
        QStringList getAllowedPropertyValues(const QString& cameraId, const QString& name);
        bool hasPropertyLimits(const QString& cameraId, const QString& name);
        double getPropertyLowerLimit(const QString& cameraId, const QString& name);
        double getPropertyUpperLimit(const QString& cameraId, const QString& name);

        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height);
        bool getLatestRaw(const QString& cameraId,
                          scopeone::core::SharedFrameHeader& header,
                          QByteArray& data);
        bool captureEventFrame(const QString& cameraId,
                               scopeone::core::SharedFrameHeader& header,
                               QByteArray& data,
                               int timeoutMs = 1500);

    signals:
        void newRawFrameReady(const QString& cameraId, const scopeone::core::SharedFrameHeader& header,
                              const QByteArray& rawData);
        void previewStateChanged(bool running);
        void agentControlServerListening(const QString& cameraId, const QString& serverName);

    private:
        struct CameraBackend;
        struct DeviceControlBackend;
        struct NativeSingleCameraBackend;
        struct AgentCameraBackend;
        struct ControlSession;
        struct CameraSlot;

        void pollSharedMemory();
        bool consumeAgentFrames(CameraSlot& slot, bool emitFrames);
        bool hasRunningCamera() const;
        void clearPropertyCaches();
        bool waitForControlReady(CameraSlot& slot, int timeoutMs);

        struct CameraSlot
        {
            QString cameraId;
            QString shmKey;
            std::shared_ptr<QSharedMemory> shm;
            std::shared_ptr<QProcess> process;
            std::shared_ptr<ControlSession> control;
            quint64 lastFrameIndex = 0;
            bool isRunning = false;
            double exposureMs = 10.0;
            std::shared_ptr<QByteArray> frameBuffer;
            scopeone::core::SharedFrameHeader cachedHeader{};
        };

        bool ensureSharedMemory(CameraSlot& slot);
        bool readLatestFrame(CameraSlot& slot);
        void updatePollingInterval();
        bool isSingleCamera(const QString& cameraId) const;
        void pollSingleCamera(CameraSlot& slot);
        bool sendControlCommand(const QString& cameraId,
                                const QJsonObject& request,
                                QJsonObject* response,
                                int timeoutMs = 1200);

        QMap<QString, std::shared_ptr<CameraSlot>> m_cameras;
        QTimer m_pollTimer;
        bool m_pollingPaused{false};
        std::shared_ptr<CMMCore> m_nativeCore;
        QString m_singleCameraId;
        std::unique_ptr<CameraBackend> m_runtime;

        QMap<QString, QString> m_propertyTypeCache;
        QMap<QString, bool> m_propertyReadOnlyCache;
        QMap<QString, QStringList> m_propertyAllowedValuesCache;
        QMap<QString, bool> m_propertyHasLimitsCache;
        QMap<QString, double> m_propertyLowerLimitCache;
        QMap<QString, double> m_propertyUpperLimitCache;
    };
}
