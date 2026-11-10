#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QReadWriteLock>
#include <QString>

#include <memory>

#include "scopeone/Device.h"
#include "scopeone/HardwareCapabilities.h"
#include "scopeone/Camera.h"
#include "internal/AcquisitionEngine.h"
#include "internal/CameraFrameDelivery.h"
#include "internal/MicroManagerProvider.h"
#include "internal/ScopeOneProvider.h"

namespace scopeone::core::internal
{
    class DaqDeviceManager;
    class SignalSourceManager;

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

        bool startPreview();
        bool stopPreview();
        bool startPreviewFor(const QString& cameraId);
        bool stopPreviewFor(const QString& cameraId);
        bool isPreviewRunning(const QString& cameraId) const;
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
        bool captureEventFrame(const QString& cameraId,
                               ImageFrame& frame,
                               int timeoutMs = 1500);
        bool supportsEventSequence(const QString& cameraId) const;
        bool captureEventSequence(const QString& cameraId,
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
        // The presets are those of Micro-Manager and of the ScopeOneConfigGroup lines, merged by name
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
        void applyScopeOneStartupPreset();
        void applyScopeOneAutoShutter();

        bool autoShutter(bool& enabled, QString* errorMessage = nullptr) const;
        bool setAutoShutter(bool enabled, QString* errorMessage = nullptr);
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

        // Where the frames of every camera arrive
        CameraFrameDelivery* frameDelivery() { return &m_frameDelivery; }
        SignalSourceManager* signalSourceManager() const { return m_signalSourceManager; }
        DaqDeviceManager* daqDeviceManager() const { return m_daqDeviceManager; }
        double pixelSizeUm(const QString& cameraId) const;

        // The Micro-Manager configuration. Loading and unloading block, so they run on a worker
        // thread, while the cameras are started and stopped on the thread of the runtime.
        QStringList microManagerAdapterSearchPaths() const;
        void setMicroManagerAdapterSearchPaths(const QStringList& paths);
        bool loadMicroManagerConfiguration(const QString& configPath,
                                           MicroManagerProvider::LoadResult& result,
                                           QString& errorMessage);
        QStringList startMicroManagerCameras(const QList<MicroManagerCameraSetup>& cameras);
        // The ScopeOne devices of the configuration, each in a DriverHost. Loading blocks, so it
        // runs on a worker thread, while the devices are registered on the thread of the runtime.
        ScopeOneProvider::LoadResult loadScopeOneDevices(const QString& configPath);
        void setConfigurationPath(const QString& path);
        QString configuredCamera() const { return m_cameraRole; }
        // False when a device conflicts with registered hardware
        bool registerScopeOneDevices();
        // The cameras of Micro-Manager and the ScopeOne devices, unregistered and to be destroyed
        // on a worker thread, as their DriverHosts take time to exit
        QList<DevicePtr> takeDriverHosts();
        // Registers the devices again, false when one conflicts with the devices of a driver
        bool setMicroManagerDevices(const QList<HardwareDeviceDescriptor>& devices);
        void stopMicroManagerPreview();
        bool unloadMicroManagerDevices(QString* errorMessage = nullptr);
        void applyMicroManagerShutdownPreset();
        void applyScopeOneShutdownPreset();
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
        bool editConfigGroups(const QStringList& groups,
                              const std::function<bool(HardwareConfiguration&, QString*)>& edit,
                              QString* errorMessage);
        bool presetSettingsForEdit(const QList<ConfigSetting>& settings,
                                   QList<HardwareConfiguration::PresetSetting>& rows,
                                   QString* errorMessage) const;
        Camera* cameraForDevice(const QString& logicalId) const;
        Device* propertyDeviceForDevice(const QString& logicalId) const;
        Stage* stageForDevice(const QString& logicalId) const;
        Shutter* shutterForDevice(const QString& logicalId) const;
        Shutter* configuredShutter(QString* deviceId) const;
        StateDevice* stateDeviceForDevice(const QString& logicalId) const;
        std::vector<ScopeOneConfigGroupLine> scopeOneConfigLines() const;
        bool applyScopeOnePreset(const QString& groupName, const QString& configName, QString* errorMessage);
        QList<QPair<CameraFrameSource*, QStringList>> frameSourcesFor(const QStringList& cameraIds) const;
        bool applyFrameDeliveryModes(const QHash<QString, FrameDeliveryMode>& modes);


        DeviceRegistry m_registry;
        std::shared_ptr<MicroManagerProvider> m_microManager;
        ScopeOneProvider m_scopeOne;
        QString m_configurationPath;
        QString m_cameraRole;
        QString m_focusRole;
        QString m_shutterRole;
        QString m_autoShutterRole;
        bool m_scopeOneShutter{false};
        SignalSourceManager* m_signalSourceManager{nullptr};
        DaqDeviceManager* m_daqDeviceManager{nullptr};

        AcquisitionEngine m_acquisitionEngine;
        CameraFrameDelivery m_frameDelivery;
    };
}
