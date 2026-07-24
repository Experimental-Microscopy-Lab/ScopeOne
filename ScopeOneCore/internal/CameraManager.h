#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

#include "internal/CameraBackend.h"

class CMMCore;

namespace scopeone::core::internal
{
    class CameraManager : public QObject
    {
        Q_OBJECT

    public:
        explicit CameraManager(QObject* parent = nullptr);
        ~CameraManager() override;

        bool configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                   const QString& cameraId,
                                   double exposureMs = 0.0);
        bool addAgentCamera(const QString& cameraId,
                            const QString& adapter,
                            const QString& device,
                            const QStringList& preInitProperties = QStringList(),
                            const QStringList& properties = QStringList(),
                            double exposureMs = 0.0);
        void shutdownNow();
        void shutdown(std::function<void(const QString&)> completion);

        bool startPreview();
        bool stopPreview();
        bool startPreviewFor(const QString& cameraId);
        bool stopPreviewFor(const QString& cameraId);
        bool isPreviewRunning(const QString& cameraId) const;
        void setFrameDeliveryPaused(bool paused);
        bool setRecordingFrameDeliveryEnabled(bool enabled);
        bool setHighRateFrameDeliveryEnabled(bool enabled);

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs);
        QStringList listProperties(const QString& cameraId);
        QString getProperty(const QString& cameraId,
                            const QString& name,
                            bool fromCache = false);
        bool setProperty(const QString& cameraId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage = nullptr);
        QString getPropertyType(const QString& cameraId, const QString& name);
        bool isPropertyReadOnly(const QString& cameraId, const QString& name);
        bool isPropertyPreInit(const QString& cameraId, const QString& name);
        QStringList getAllowedPropertyValues(const QString& cameraId, const QString& name);
        bool hasPropertyLimits(const QString& cameraId, const QString& name);
        double getPropertyLowerLimit(const QString& cameraId, const QString& name);
        double getPropertyUpperLimit(const QString& cameraId, const QString& name);

        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height);
        bool captureEventFrame(const QString& cameraId,
                               scopeone::core::ImageFrame& frame,
                               int timeoutMs = 1500);

    signals:
        void newRawFrameReady(const scopeone::core::ImageFrame& frame);
        void rawFramesAcquired(const QString& cameraId, quint64 frameCount);
        void recordingFramesReady(const QList<scopeone::core::ImageFrame>& frames);
        void frameDeliveryFailed(const QString& errorMessage);
        void previewStateChanged(bool running);
        void agentControlServerListening(const QString& cameraId, const QString& serverName);

    private:
        bool activateBackend(CameraBackend::Kind kind);

        std::unique_ptr<CameraBackend> m_backend;
        bool m_recordingFrameDeliveryEnabled{false};
        bool m_highRateFrameDeliveryEnabled{false};
        QMap<QString, CameraPropertyReadback> m_propertyDetailsCache;
    };
}
