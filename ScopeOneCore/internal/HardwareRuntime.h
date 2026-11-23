#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QReadWriteLock>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

#include "scopeone/Device.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/Camera.h"
#include "scopeone/CameraSettings.h"
#include "scopeone/ExperimentDocument.h"
#include "scopeone/HardwareConfiguration.h"
#include "internal/AcquisitionEngine.h"
#include "internal/CameraFrameDelivery.h"
#include "internal/ScopeOneProvider.h"

namespace scopeone::core::internal
{
    class DaqDeviceManager;
    class MicroManagerProvider;
    class SignalSourceManager;

    // The devices that loading a cfg file gave, over every provider
    struct ConfigurationLoadResult
    {
        QStringList cameraIds;
        QStringList failedDevices;
        // The cameras that did not start, which fail the load
        QStringList failedCameras;
        int successCount{0};
        int failCount{0};
        int skippedCameraCount{0};
        bool foundCamera{false};
        QList<HardwareDeviceDescriptor> devices;
    };

    // The values that one device property takes on successive triggers of the camera
    struct PropertySequence
    {
        QString device;
        QString property;
        std::vector<QString> values;
    };

    class DeviceRegistry : public QObject
    {
        Q_OBJECT

    public:
        explicit DeviceRegistry(QObject* parent = nullptr);

        void clear();
        bool registerProvider(const DevicePtr& provider,
                              const HardwareProviderDescriptor& descriptor,
                              const QList<HardwareDeviceDescriptor>& devices);
        void unregisterProvider(const QString& providerId);
        QList<HardwareProviderDescriptor> providers() const;
        QList<HardwareDeviceDescriptor> devices() const;
        HardwareDeviceDescriptor device(const QString& logicalId) const;
        DevicePtr provider(const QString& providerId) const;
        DevicePtr providerForDevice(const QString& logicalId) const;

    signals:
        void changed();

    private:
        struct ProviderEntry
        {
            DevicePtr provider;
            HardwareProviderDescriptor descriptor;
            QList<HardwareDeviceDescriptor> devices;
        };

        mutable QReadWriteLock m_lock;
        QHash<QString, ProviderEntry> m_providers;
    };

    // The hardware of every provider, for the parts of ScopeOne above the providers, which reach
    // the hardware only through it. It routes each call to the provider of the device, and the
    // calls that span providers, such as "All" cameras or the default stages, it answers itself.
    // Every provider reaches it through the same interfaces, those of the driver SDK.
    class HardwareRuntime : public QObject
    {
        Q_OBJECT

    public:
        explicit HardwareRuntime(QObject* parent = nullptr);

        // One camera, or all cameras for "All"
        bool startPreview(const QString& cameraIdOrAll);
        bool stopPreview(const QString& cameraIdOrAll);
        bool isPreviewRunning(const QString& cameraId) const;
        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs, QString* errorMessage = nullptr);
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
        QStringList getAllowedPropertyValues(const QString& cameraId,
                                             const QString& name);
        bool hasPropertyLimits(const QString& cameraId, const QString& name);
        double getPropertyLowerLimit(const QString& cameraId, const QString& name);
        double getPropertyUpperLimit(const QString& cameraId, const QString& name);
        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId,
                    int& x,
                    int& y,
                    int& width,
                    int& height);
        bool snap(const QString& cameraId,
                  ImageFrame& frame,
                  int timeoutMs = 1500);
        bool supportsSequence(const QString& cameraId) const;
        bool snapSequence(const QString& cameraId,
                          int count,
                          std::vector<ImageFrame>& frames,
                          int timeoutMs = 10000);
        void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused);
        bool setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                              bool enabled);
        bool setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                             bool enabled);
        bool isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token);
        void finishProcessingFrame(const QString& cameraId, quint64 token);
        QString defaultXYStage() const;
        QString defaultZStage() const;
        bool getXYPosition(const QString& deviceId,
                           double& x,
                           double& y,
                           QString* errorMessage = nullptr) const;
        bool getZPosition(const QString& deviceId,
                          double& z,
                          QString* errorMessage = nullptr) const;
        bool setRelativeXYPosition(const QString& deviceId,
                                   double dx,
                                   double dy,
                                   QString* errorMessage = nullptr);
        bool setRelativeZPosition(const QString& deviceId,
                                  double dz,
                                  QString* errorMessage = nullptr);
        bool setXYPosition(const QString& deviceId,
                           double x,
                           double y,
                           QString* errorMessage = nullptr);
        bool setZPosition(const QString& deviceId,
                          double z,
                          QString* errorMessage = nullptr);
        bool isShutterOpen(const QString& deviceId,
                           bool& open,
                           QString* errorMessage = nullptr) const;
        bool setShutterOpen(const QString& deviceId,
                            bool open,
                            QString* errorMessage = nullptr);
        bool getState(const QString& deviceId,
                      long& state,
                      QString* errorMessage = nullptr) const;
        bool setState(const QString& deviceId,
                      long state,
                      QString* errorMessage = nullptr);
        QString stateLabel(const QString& deviceId, long state) const;
        std::vector<LightSourceLine> lightSources() const;
        bool setLightSource(const LightSourceLine& source, QString* errorMessage);
        bool reloadRoles(QString* errorMessage);
        QStringList availableConfigGroups() const;
        QStringList availableConfigs(const QString& groupName) const;
        QString currentConfig(const QString& groupName) const;
        std::vector<scopeone::core::ConfigSetting> configSettings(
            const QString& groupName, const QString& configName) const;
        // The presets of every provider, merged by name
        bool setConfig(const QString& groupName,
                       const QString& configName,
                       QString* errorMessage = nullptr);
        bool addConfigGroup(const QString& group, QString* errorMessage = nullptr);
        bool removeConfigGroup(const QString& group, QString* errorMessage = nullptr);
        bool renameConfigGroup(const QString& group, const QString& newGroup, QString* errorMessage = nullptr);
        bool setConfigPreset(const QString& group, const QString& preset,
                             const std::vector<scopeone::core::ConfigSetting>& settings,
                             QString* errorMessage = nullptr);
        bool removeConfigPreset(const QString& group, const QString& preset, QString* errorMessage = nullptr);
        bool renameConfigPreset(const QString& group, const QString& preset, const QString& newPreset,
                                QString* errorMessage = nullptr);
        bool setConfigGroupProperties(const QString& group, const QList<ConfigSetting>& properties,
                                      QString* errorMessage = nullptr);
        // The startup settings of the cfg after a load, and the roles the providers hold. The
        // runtime opens and closes the shutter around the images itself.
        void applyStartupSettings();
        // The shutdown settings of the cfg, before the devices are released
        void applyShutdownSettings();

        bool autoShutter() const { return m_autoShutter; }
        void setAutoShutter(bool enabled) { m_autoShutter = enabled; }
        QString defaultShutter() const;
        // Opens or closes the shutter that lights the sample for the cameras
        bool setDefaultShutterOpen(bool open, QString* errorMessage = nullptr);
        bool hasAutofocus() const;
        bool runAutofocus(QString* errorMessage = nullptr);
        long focusSequenceCapacity(const QString& focusDevice) const;
        bool startFocusSequence(const QString& focusDevice,
                                const std::vector<double>& positions,
                                QString* errorMessage = nullptr);
        void stopFocusSequence(const QString& focusDevice);
        long propertySequenceCapacity(const QString& device, const QString& property) const;
        bool startPropertySequences(const std::vector<PropertySequence>& sequences,
                                    QString* errorMessage = nullptr);
        void stopPropertySequences(const std::vector<PropertySequence>& sequences);

        // The ScopeOneLimit lines of the cfg. Every command that sets a limited property, exposure or
        // stage position is refused as a whole when a value leaves its range.
        QList<HardwareConfiguration::Limit> limits() const;
        bool checkLimit(const QString& device, const QString& property, double value,
                        QString* errorMessage) const;
        bool checkXYLimit(const QString& device, double x, double y, QString* errorMessage) const;
        // Refuses a plan whose exposures, positions, Z positions or channel presets leave a limit,
        // before anything moves. The shift of the autofocus is checked as each move is made.
        bool checkPlanLimits(const ExperimentPlan& plan, QString* errorMessage) const;

        // Where the frames of every camera arrive
        CameraFrameDelivery* frameDelivery() { return &m_frameDelivery; }
        SignalSourceManager* signalSourceManager() const { return m_signalSourceManager; }
        DaqDeviceManager* daqDeviceManager() const { return m_daqDeviceManager; }
        double pixelSizeUm(const QString& cameraId) const;
        // The camera in ScopeOne's terms, with properties its entry of the device property snapshot
        CameraSettings cameraSettings(const QString& cameraId, const QJsonObject& properties) const;

        // The folders of the device adapters besides the one of the application
        QStringList deviceAdapterSearchPaths() const;
        void setDeviceAdapterSearchPaths(const QStringList& paths);
        // Loads the devices of the cfg file and starts the cameras. It blocks, so it runs on a
        // worker thread, while the devices are registered on the thread of the runtime. A failed
        // load leaves no device loaded.
        bool loadConfiguration(const QString& configPath, ConfigurationLoadResult& result,
                               QString& errorMessage);
        // False when a device conflicts with registered hardware
        bool registerLoadedDevices();
        void setConfigurationPath(const QString& path);
        QString configuredCamera() const { return m_cameraRole; }
        // The cameras and devices that run in DriverHosts, unregistered and to be destroyed on a
        // worker thread, as their DriverHosts take time to exit
        QList<DevicePtr> takeDriverHosts();
        void stopAllPreviews();
        // Takes the devices that stay loaded until unloadDevices out of the registry
        void clearLoadedDevices();
        bool unloadDevices(QString* errorMessage = nullptr);
        QList<HardwareDeviceDescriptor> devices() const { return m_registry.devices(); }
        static QString microManagerVersion();
        void clear();
        bool registerProvider(const DevicePtr& provider);
        void unregisterProvider(const QString& providerId);
        void stopPreviewForProvider(const QString& providerId);

    signals:
        void frameReady(const ImageFrame& frame);
        void devicesChanged();
        void previewStateChanged(bool running);

    private:
        // Takes the roles and the limits of the cfg
        void applyRoles(const HardwareConfiguration& document);
        std::optional<HardwareConfiguration::Limit> limit(const QString& device, const QString& property) const;
        // A property value with a limit has to be a number inside it
        bool checkPropertyLimit(const QString& device, const QString& property, const QString& value,
                                QString* errorMessage) const;
        bool editConfigGroups(const QStringList& groups,
                              const std::function<bool(HardwareConfiguration&, QString*)>& edit,
                              QString* errorMessage);
        bool presetSettingsForEdit(const QList<ConfigSetting>& settings, QString* errorMessage) const;
        Camera* cameraForDevice(const QString& logicalId) const;
        Device* propertyDeviceForDevice(const QString& logicalId) const;
        XYStage* xyStageForDevice(const QString& logicalId) const;
        ZStage* zStageForDevice(const QString& logicalId) const;
        Shutter* shutterForDevice(const QString& logicalId) const;
        Shutter* configuredShutter(QString* deviceId) const;
        Autofocus* configuredAutofocus(QString* deviceId) const;
        QString roleDevice(const QString& role, const QString& configured, HardwareDeviceKind kind) const;
        bool anyCameraRunning() const;
        StateDevice* stateDeviceForDevice(const QString& logicalId) const;
        std::vector<ScopeOneConfigGroupLine> scopeOneConfigLines() const;
        // Sets the lines of loaded devices, stopping at the first property that cannot be set
        bool applyScopeOneLines(const QList<ScopeOneConfigGroupLine>& lines, QString* errorMessage);
        QList<QPair<CameraFrameSource*, QStringList>> frameSourcesFor(const QStringList& cameraIds) const;
        bool applyFrameDeliveryModes(const QHash<QString, FrameDeliveryMode>& modes);


        DeviceRegistry m_registry;
        std::shared_ptr<MicroManagerProvider> m_microManager;
        ScopeOneProvider m_scopeOne;
        QString m_configurationPath;
        QString m_cameraRole;
        QString m_xyStageRole;
        QString m_focusRole;
        QString m_shutterRole;
        QString m_autofocusRole;
        std::atomic<bool> m_autoShutter{true};
        // Read on the hardware and MDA threads, set when a cfg is loaded or edited
        mutable QReadWriteLock m_limitsLock;
        QList<HardwareConfiguration::Limit> m_limits;
        SignalSourceManager* m_signalSourceManager{nullptr};
        DaqDeviceManager* m_daqDeviceManager{nullptr};

        AcquisitionEngine m_acquisitionEngine;
        CameraFrameDelivery m_frameDelivery;
    };
}
