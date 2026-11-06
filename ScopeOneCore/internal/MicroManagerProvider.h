#pragma once

#include <QList>

#include <memory>

#include <QMap>
#include <QReadWriteLock>

#include <atomic>

#include "internal/CameraFrameDelivery.h"
#include "internal/MicroManagerCamera.h"
#include "scopeone/HardwareProvider.h"
#include "scopeone/CameraProvider.h"

class CMMCore;

namespace scopeone::core::internal
{
    // The devices of the loaded Micro-Manager configuration. They run on the core of ScopeOne,
    // except the cameras of a configuration with more than one camera: each of those runs in a
    // DriverHost of its own, so two cameras of the same model do not share one vendor library.
    class MicroManagerProvider final : public HardwareProvider,
                                       public CameraProvider,
                                       public StageProvider,
                                       public ShutterProvider,
                                       public AutofocusProvider,
                                       public StateProvider,
                                       public ConfigurationProvider,
                                       public CameraFrameSource
    {
    public:
        struct LoadResult
        {
            QStringList cameraIds;
            QList<MicroManagerCameraSetup> cameras;
            QStringList failedDevices;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
            bool useSingleCamera{false};
            QList<HardwareDeviceDescriptor> devices;
        };

        MicroManagerProvider();
        ~MicroManagerProvider() override;

        static QString coreVersion();

        QStringList adapterSearchPaths() const { return m_adapterSearchPaths; }
        void setAdapterSearchPaths(const QStringList& paths) { m_adapterSearchPaths = paths; }
        // Blocks while the devices initialize
        bool loadConfiguration(const QString& configPath, LoadResult& result, QString& errorMessage);
        bool unloadDevices(QString* errorMessage);
        void applyShutdownPreset();

        HardwareProviderDescriptor descriptor() const override;
        QList<HardwareDeviceDescriptor> devices() const override;
        void setDevices(const QList<HardwareDeviceDescriptor>& devices);

        // Starts the cameras of a loaded configuration and returns the ones that failed. A single
        // camera has to be loaded on the core already, while more are loaded again in their
        // DriverHosts.
        QStringList startCameras(const QList<MicroManagerCameraSetup>& cameras);
        // Hands over the cameras, so they can be stopped away from the calling thread
        QList<HardwareProviderPtr> takeCameras();

        void setFrameDelivery(CameraFrameDelivery* delivery) override;
        bool setFrameDeliveryMode(const QStringList& cameraIds, FrameDeliveryMode mode) override;
        void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused) override;

        void setFrameSink(FrameSink sink) override;
        void setPreviewStateSink(PreviewStateSink sink) override;
        bool startPreviewFor(const QString& cameraId) override;
        bool stopPreviewFor(const QString& cameraId) override;
        bool isPreviewRunning(const QString& cameraId) const override;
        bool getExposure(const QString& cameraId, double& exposureMs) const override;
        bool setExposure(const QString& cameraId, double exposureMs) override;
        QStringList listProperties(const QString& cameraId) override;
        QString getProperty(const QString& cameraId,
                            const QString& name,
                            bool fromCache) override;
        bool setProperty(const QString& cameraId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override;
        QString getPropertyType(const QString& cameraId, const QString& name) override;
        bool isPropertyReadOnly(const QString& cameraId, const QString& name) override;
        bool isPropertyPreInit(const QString& cameraId, const QString& name) override;
        QStringList getAllowedPropertyValues(const QString& cameraId,
                                             const QString& name) override;
        bool hasPropertyLimits(const QString& cameraId, const QString& name) override;
        double getPropertyLowerLimit(const QString& cameraId, const QString& name) override;
        double getPropertyUpperLimit(const QString& cameraId, const QString& name) override;
        bool setROI(const QString& cameraId, int x, int y, int width, int height) override;
        bool clearROI(const QString& cameraId) override;
        bool getROI(const QString& cameraId,
                    int& x,
                    int& y,
                    int& width,
                    int& height) override;
        bool captureEventFrame(const QString& cameraId,
                               scopeone::core::ImageFrame& frame,
                               int timeoutMs) override;
        bool supportsEventSequence(const QString& cameraId) const override;
        bool captureEventSequence(const QString& cameraId,
                                  int count,
                                  std::vector<scopeone::core::ImageFrame>& frames,
                                  int timeoutMs) override;
        QString defaultXYStage() const override;
        QString defaultZStage() const override;
        bool getXYPosition(const QString& deviceId,
                           double& x,
                           double& y,
                           QString* errorMessage) const override;
        bool getZPosition(const QString& deviceId,
                          double& z,
                          QString* errorMessage) const override;
        bool setRelativeXYPosition(const QString& deviceId,
                                   double dx,
                                   double dy,
                                   QString* errorMessage) override;
        bool setRelativeZPosition(const QString& deviceId,
                                  double dz,
                                  QString* errorMessage) override;
        bool setXYPosition(const QString& deviceId,
                           double x,
                           double y,
                           QString* errorMessage) override;
        bool setZPosition(const QString& deviceId,
                          double z,
                          QString* errorMessage) override;
        bool isShutterOpen(const QString& deviceId,
                           bool& open,
                           QString* errorMessage) const override;
        bool setShutterOpen(const QString& deviceId,
                            bool open,
                            QString* errorMessage) override;
        bool getState(const QString& deviceId,
                      long& state,
                      QString* errorMessage) const override;
        bool setState(const QString& deviceId,
                      long state,
                      QString* errorMessage) override;
        QString stateLabel(const QString& deviceId, long state) const override;
        QStringList availableConfigGroups() const override;
        QStringList availableConfigs(const QString& groupName) const override;
        QString currentConfig(const QString& groupName) const override;
        std::vector<scopeone::core::ConfigSetting> configSettings(
            const QString& groupName, const QString& configName) const override;
        bool setConfig(const QString& groupName,
                       const QString& configName,
                       QString* errorMessage) override;

        QString defaultShutter() const override;
        bool autoShutter() const override;
        bool setAutoShutter(bool enabled, QString* errorMessage) override;
        QString defaultAutofocus() const override;
        bool runAutofocus(const QString& deviceId, QString* errorMessage) override;
        long stageSequenceCapacity(const QString& deviceId) const override;
        bool startStageSequence(const QString& deviceId,
                                const std::vector<double>& positions,
                                QString* errorMessage) override;
        void stopStageSequence(const QString& deviceId) override;
        long propertySequenceCapacity(const QString& deviceId, const QString& name) override;
        bool startPropertySequence(const QString& deviceId,
                                   const QString& name,
                                   const QStringList& values,
                                   QString* errorMessage) override;
        void stopPropertySequence(const QString& deviceId, const QString& name) override;
        double pixelSizeUm(const QString& cameraId) const override;

    private:
        struct Camera
        {
            // A MicroManagerCamera on the core of ScopeOne, or the DriverHost of the camera
            HardwareProviderPtr provider;
            CameraProvider* camera{nullptr};
            CameraFrameSource* frameSource{nullptr};
            std::shared_ptr<MicroManagerCamera> local;
            // The index of the last frame the local camera delivered, 0 after a change of mode
            std::shared_ptr<std::atomic<quint64>> lastFrameIndex;
        };

        // A camera held while a call uses it, so unloading the configuration on another thread
        // cannot free it meanwhile
        struct CameraRef
        {
            HardwareProviderPtr owner;
            CameraProvider* camera{nullptr};
            explicit operator bool() const { return camera != nullptr; }
            CameraProvider* operator->() const { return camera; }
        };

        bool startLocalCamera(const QString& cameraId);
        CameraRef camera(const QString& cameraId) const;
        Camera cameraEntry(const QString& cameraId) const;
        void readBinning(const QString& cameraId);
        QList<Camera> cameras() const;

        std::shared_ptr<CMMCore> m_core;
        QStringList m_adapterSearchPaths;
        QList<HardwareDeviceDescriptor> m_devices;
        // Written when a configuration loads or unloads, read by calls on any thread
        mutable QReadWriteLock m_camerasLock;
        QMap<QString, Camera> m_cameras;
        // The binning of each camera in a DriverHost, read when it changes, as the core of
        // ScopeOne does not hold these cameras to apply it to the pixel size
        QHash<QString, int> m_binnings;
        std::atomic<CameraFrameDelivery*> m_delivery{nullptr};
        PreviewStateSink m_previewStateSink;
    };
}
