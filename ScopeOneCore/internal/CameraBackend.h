#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

#include "scopeone/ImageFrame.h"

class CMMCore;

namespace scopeone::core::internal
{
    struct CameraPropertyReadback
    {
        QString value;
        QString type{QStringLiteral("Unknown")};
        bool readOnly{true};
        bool preInit{false};
        QStringList allowedValues;
        bool hasLimits{false};
        double lowerLimit{0.0};
        double upperLimit{0.0};
    };

    class CameraBackend : public QObject
    {
        Q_OBJECT

    public:
        enum class Kind
        {
            Native,
            Agent
        };

        explicit CameraBackend(QObject* parent = nullptr);
        ~CameraBackend() override;

        virtual Kind kind() const = 0;
        virtual void shutdown(std::function<void(const QString&)> completion)
        {
            if (completion)
            {
                completion({});
            }
        }

        virtual bool configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                           const QString& cameraId,
                                           double exposureMs);
        virtual bool addAgentCamera(const QString& cameraId,
                                    const QString& adapter,
                                    const QString& device,
                                    const QStringList& preInitProperties,
                                    const QStringList& properties,
                                    double exposureMs);

        virtual bool startPreview() = 0;
        virtual bool stopPreview() = 0;
        virtual bool startPreviewFor(const QString& cameraId) = 0;
        virtual bool stopPreviewFor(const QString& cameraId) = 0;
        virtual bool isPreviewRunning(const QString& cameraId) const = 0;
        virtual void setFrameDeliveryPaused(bool paused) = 0;
        virtual bool captureEventFrame(const QString& cameraId,
                                       scopeone::core::ImageFrame& frame,
                                       int timeoutMs);

        virtual bool setRecordingFrameDeliveryEnabled(bool enabled);
        virtual bool setHighRateFrameDeliveryEnabled(bool enabled);

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs);
        QStringList listProperties(const QString& cameraId);
        bool readPropertyDetails(const QString& cameraId,
                                 const QString& name,
                                 bool fromCache,
                                 CameraPropertyReadback& readback);
        bool setProperty(const QString& cameraId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage);
        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height);

    signals:
        void rawFrameReady(const scopeone::core::ImageFrame& frame);
        void rawFramesAcquired(const QString& cameraId, quint64 frameCount);
        void recordingFramesReady(const QList<scopeone::core::ImageFrame>& frames);
        void frameDeliveryFailed(const QString& errorMessage);
        void previewStateChanged(bool running);
        void agentControlServerListening(const QString& cameraId, const QString& serverName);

    protected:
        bool applyWithPreviewRestart(const QString& cameraId, const std::function<bool()>& operation);
        void notifyPreviewStarted(const QString& cameraId);
        void notifyPreviewStopped();
        void submitFrames(const QList<scopeone::core::ImageFrame>& frames, quint64 acquiredFrameCount);
        void discardPendingPreviewFrames();
        bool recordingFrameDeliveryEnabled() const;
        bool highRateFrameDeliveryEnabled() const;

        virtual bool hasRunningCamera() const = 0;
        virtual bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const = 0;
        virtual QStringList resolveTargetCameraIds(const QString& cameraIdOrAll) const = 0;
        virtual bool readExposureFor(const QString& cameraId, double& exposureMs) const = 0;
        virtual bool writeExposureFor(const QString& cameraId, double exposureMs) = 0;
        virtual QStringList listPropertiesFor(const QString& cameraId) = 0;
        virtual bool readPropertyDetailsFor(const QString& cameraId,
                                            const QString& name,
                                            bool fromCache,
                                            CameraPropertyReadback& readback) = 0;
        virtual bool setPropertyFor(const QString& cameraId,
                                    const QString& name,
                                    const QString& value,
                                    QString* errorMessage) = 0;
        virtual bool setROIFor(const QString& cameraId, int x, int y, int width, int height) = 0;
        virtual bool clearROIFor(const QString& cameraId) = 0;
        virtual bool getROIFor(const QString& cameraId,
                               int& x,
                               int& y,
                               int& width,
                               int& height) = 0;

    private:
        void flushPendingFrames();

        mutable QMutex m_frameDeliveryMutex;
        QHash<QString, scopeone::core::ImageFrame> m_pendingLatestFrames;
        QHash<QString, quint64> m_pendingAcquiredFrameCounts;
        QList<scopeone::core::ImageFrame> m_pendingRecordingFrames;
        qint64 m_pendingRecordingBytes{0};
        QString m_pendingDeliveryError;
        std::atomic_bool m_recordingFrameDeliveryEnabled{false};
        std::atomic_bool m_highRateFrameDeliveryEnabled{false};
        bool m_frameFlushQueued{false};
    };

    std::unique_ptr<CameraBackend> createNativeCameraBackend();
    std::unique_ptr<CameraBackend> createAgentCameraBackend();
}
