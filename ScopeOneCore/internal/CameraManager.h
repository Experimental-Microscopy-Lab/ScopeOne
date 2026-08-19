#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

#include "internal/CameraBackend.h"
#include "scopeone/CameraProvider.h"
#include "internal/CameraRuntimeControl.h"

class CMMCore;

namespace scopeone::core::internal
{
    class CameraManager : public QObject, public CameraProvider, public CameraRuntimeControl
    {
        Q_OBJECT

    public:
        explicit CameraManager(QObject* parent = nullptr);
        ~CameraManager() override;

        void setFrameSink(FrameSink sink) override;

        bool configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                   const QString& cameraId,
                                   double exposureMs = 0.0);
        bool addDriverHostCamera(const QString& cameraId,
                            const QString& adapter,
                            const QString& device,
                            const QStringList& preInitProperties = QStringList(),
                            const QStringList& properties = QStringList(),
                            double exposureMs = 0.0);
        void shutdownNow();
        void shutdown(std::function<void(const QString&)> completion);

        bool startPreview() override;
        bool stopPreview() override;
        bool usesDriverHostBackend() const;
        bool startPreviewFor(const QString& cameraId) override;
        bool stopPreviewFor(const QString& cameraId) override;
        bool isPreviewRunning(const QString& cameraId) const override;
        void setFrameDeliveryPaused(bool paused) override;
        bool setRecordingFrameDeliveryEnabled(bool enabled) override;
        bool setHighRateFrameDeliveryEnabled(bool enabled) override;
        bool isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token) override;
        void finishProcessingFrame(const QString& cameraId, quint64 token) override;

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const override;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs) override;
        QStringList listProperties(const QString& cameraId) override;
        QString getProperty(const QString& cameraId,
                            const QString& name,
                            bool fromCache = false) override;
        bool setProperty(const QString& cameraId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage = nullptr) override;
        QString getPropertyType(const QString& cameraId, const QString& name) override;
        bool isPropertyReadOnly(const QString& cameraId, const QString& name) override;
        bool isPropertyPreInit(const QString& cameraId, const QString& name) override;
        QStringList getAllowedPropertyValues(const QString& cameraId, const QString& name) override;
        bool hasPropertyLimits(const QString& cameraId, const QString& name) override;
        double getPropertyLowerLimit(const QString& cameraId, const QString& name) override;
        double getPropertyUpperLimit(const QString& cameraId, const QString& name) override;

        bool setROI(const QString& cameraId, int x, int y, int width, int height) override;
        bool clearROI(const QString& cameraId) override;
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) override;
        bool captureEventFrame(const QString& cameraId,
                               scopeone::core::ImageFrame& frame,
                               int timeoutMs = 1500) override;

    signals:
        void processingFrameReady(const scopeone::core::ImageFrame& frame, quint64 token);
        void rawFramesAcquired(const QString& cameraId, quint64 frameCount);
        void recordingFramesReady(const QList<scopeone::core::ImageFrame>& frames);
        void frameDeliveryFailed(const QString& errorMessage, quint64 droppedFrames);
        void previewStateChanged(bool running);
        void driverHostControlServerListening(const QString& cameraId, const QString& serverName);

    private:
        bool activateBackend(CameraBackend::Kind kind);

        ProcessingFrameGate m_processingFrameGate;
        std::unique_ptr<CameraBackend> m_backend;
        FrameSink m_frameSink;
        bool m_recordingFrameDeliveryEnabled{false};
        bool m_highRateFrameDeliveryEnabled{false};
        QMap<QString, QStringList> m_propertyNamesCache;
        QMap<QString, CameraPropertyReadback> m_propertyDetailsCache;
    };
}
