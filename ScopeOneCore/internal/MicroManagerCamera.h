#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

#include "internal/FrameDeliveryMode.h"
#include "scopeone/Camera.h"
#include "scopeone/Device.h"

class CMMCore;

namespace scopeone::core::internal
{
    // What a camera of a Micro-Manager configuration needs to be loaded again on a core of its own
    struct MicroManagerCameraSetup
    {
        QString cameraId;
        QString adapter;
        QString device;
        // {"name", "value"} objects, applied before and after the camera is initialized
        QJsonArray preInitProperties;
        QJsonArray properties;
        double exposureMs{0.0};

        QJsonObject toJson() const;
        static MicroManagerCameraSetup fromJson(const QJsonObject& object);
    };

    class MicroManagerCameraPoller;

    // One Micro-Manager camera on a CMMCore. A configuration with one camera runs it on the core of
    // ScopeOne. A configuration with more runs each camera in a DriverHost of its own, on a core that
    // holds only that camera, because a vendor library loaded twice in one process shares its state
    // and two cameras of the same model would stop each other.
    class MicroManagerCamera final : public Device, public Camera
    {
    public:
        // A camera that lends its frames hands out frames that point into the circular buffer of
        // the core. Such a frame is valid only during the call of the frame sink.
        MicroManagerCamera(std::shared_ptr<CMMCore> core, QString cameraId, bool lendFrames);
        ~MicroManagerCamera() override;

        // Loads the camera on a new core that holds only this camera
        static std::shared_ptr<MicroManagerCamera> load(const MicroManagerCameraSetup& setup,
                                                        bool lendFrames,
                                                        QString* errorMessage);

        QString cameraId() const { return m_cameraId; }

        HardwareProviderDescriptor descriptor() const override;
        QList<HardwareDeviceDescriptor> devices() const override;

        void setFrameSink(FrameSink sink) override;
        void setPreviewStateSink(PreviewStateSink sink) override;
        // Called with the error when the running acquisition fails
        void setErrorSink(std::function<void(const QString&)> sink);
        // Asked in LatestOnly mode before the newest frame of a poll is copied
        void setFrameFilter(std::function<bool()> filter);
        void setFrameDeliveryMode(FrameDeliveryMode mode);
        // A pause stops reading the circular buffer, and its end restarts a live acquisition that
        // an event capture stopped
        void setFrameDeliveryPaused(bool paused);

        bool startPreview(const QString& cameraId) override;
        bool stopPreview(const QString& cameraId) override;
        bool isPreviewRunning(const QString& cameraId) const override;
        bool getExposure(const QString& cameraId, double& exposureMs) const override;
        bool setExposure(const QString& cameraId, double exposureMs) override;
        bool setROI(const QString& cameraId, int x, int y, int width, int height) override;
        bool clearROI(const QString& cameraId) override;
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) override;
        bool snap(const QString& cameraId, ImageFrame& frame, int timeoutMs) override;
        bool supportsSequence(const QString& cameraId) const override;
        bool snapSequence(const QString& cameraId,
                          int count,
                          std::vector<ImageFrame>& frames,
                          int timeoutMs) override;

        QStringList listProperties(const QString& deviceId) override;
        QString getProperty(const QString& deviceId, const QString& name, bool fromCache) override;
        bool setProperty(const QString& deviceId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override;
        QString getPropertyType(const QString& deviceId, const QString& name) override;
        bool isPropertyReadOnly(const QString& deviceId, const QString& name) override;
        bool isPropertyPreInit(const QString& deviceId, const QString& name) override;
        QStringList getAllowedPropertyValues(const QString& deviceId, const QString& name) override;
        bool hasPropertyLimits(const QString& deviceId, const QString& name) override;
        double getPropertyLowerLimit(const QString& deviceId, const QString& name) override;
        double getPropertyUpperLimit(const QString& deviceId, const QString& name) override;
        long propertySequenceCapacity(const QString& deviceId, const QString& name) override;
        bool startPropertySequence(const QString& deviceId,
                                   const QString& name,
                                   const QStringList& values,
                                   QString* errorMessage) override;
        void stopPropertySequence(const QString& deviceId, const QString& name) override;

    private:
        friend class MicroManagerCameraPoller;

        bool matches(const QString& cameraId) const;
        bool startAcquisition();
        void stopAcquisition();
        void setPreviewRunning(bool running);
        // Stops the live acquisition before the core takes an image of its own
        void suspendAcquisition();
        bool makeFrame(const void* pixels, ImageFrame& frame);
        void handleAcquisitionFailure(const QString& error);

        std::shared_ptr<CMMCore> m_core;
        QString m_cameraId;
        bool m_lendFrames{false};
        std::atomic<quint64> m_frameIndex{0};
        std::atomic_bool m_running{false};
        bool m_paused{false};
        std::atomic_bool m_suspended{false};
        mutable QMutex m_sinkMutex;
        FrameSink m_frameSink;
        PreviewStateSink m_previewStateSink;
        std::function<void(const QString&)> m_errorSink;
        std::function<bool()> m_frameFilter;
        QThread m_pollThread;
        MicroManagerCameraPoller* m_poller{nullptr};
    };
}
