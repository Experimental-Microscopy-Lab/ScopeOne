#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QReadWriteLock>
#include <QString>

#include <memory>

#include "scopeone/HardwareProvider.h"
#include "scopeone/CameraProvider.h"
#include "internal/AcquisitionEngine.h"
#include "internal/CameraRuntimeControl.h"
#include "internal/FrameRouter.h"

namespace scopeone::core::internal
{
    class DeviceRegistry : public QObject
    {
        Q_OBJECT

    public:
        explicit DeviceRegistry(QObject* parent = nullptr);

        void clear();
        bool registerProvider(const HardwareProviderPtr& provider,
                              const HardwareProviderDescriptor& descriptor,
                              const QList<HardwareDeviceDescriptor>& devices);
        void unregisterProvider(const QString& providerId);
        QList<HardwareProviderDescriptor> providers() const;
        QList<HardwareDeviceDescriptor> devices() const;
        HardwareDeviceDescriptor device(const QString& logicalId) const;
        HardwareProviderPtr provider(const QString& providerId) const;
        HardwareProviderPtr providerForDevice(const QString& logicalId) const;

    signals:
        void changed();

    private:
        struct ProviderEntry
        {
            HardwareProviderPtr provider;
            HardwareProviderDescriptor descriptor;
            QList<HardwareDeviceDescriptor> devices;
        };

        mutable QReadWriteLock m_lock;
        QHash<QString, ProviderEntry> m_providers;
    };

    class HardwareRuntime : public QObject,
                            public CameraProvider,
                            public StageProvider,
                            public ShutterProvider,
                            public StateProvider,
                            public ConfigurationProvider,
                            public CameraRuntimeControl
    {
        Q_OBJECT

    public:
        explicit HardwareRuntime(QObject* parent = nullptr);

        void setFrameSink(FrameSink sink) override;
        void setPreviewStateSink(PreviewStateSink sink) override;
        bool startPreview() override;
        bool stopPreview() override;
        bool startPreviewFor(const QString& cameraId) override;
        bool stopPreviewFor(const QString& cameraId) override;
        bool isPreviewRunning(const QString& cameraId) const override;
        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const override;
        bool setExposure(const QString& cameraIdOrAll, double exposureMs) override;
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
                               ImageFrame& frame,
                               int timeoutMs) override;
        void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused) override;
        bool setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                              bool enabled) override;
        bool setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                             bool enabled) override;
        bool isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token) override;
        void finishProcessingFrame(const QString& cameraId, quint64 token) override;
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
        bool setConfig(const QString& groupName,
                       const QString& configName,
                       QString* errorMessage) override;

        DeviceRegistry* deviceRegistry() { return &m_registry; }
        const DeviceRegistry* deviceRegistry() const { return &m_registry; }
        FrameRouter* frameRouter() { return &m_frameRouter; }
        void clear();
        bool registerProvider(const HardwareProviderPtr& provider);
        void unregisterProvider(const QString& providerId);
        bool refreshProvider(const QString& providerId);
        bool stopPreviewForProvider(const QString& providerId);

    signals:
        void devicesChanged();
        void previewStateChanged(bool running);

    private:
        CameraProvider* cameraProviderForDevice(const QString& logicalId) const;
        DevicePropertyProvider* propertyProviderForDevice(const QString& logicalId) const;
        StageProvider* stageProviderForDevice(const QString& logicalId) const;
        ShutterProvider* shutterProviderForDevice(const QString& logicalId) const;
        StateProvider* stateProviderForDevice(const QString& logicalId) const;
        ConfigurationProvider* configurationProviderForGroup(const QString& groupName) const;
        QList<CameraProvider*> cameraProviders() const;
        QList<QPair<CameraRuntimeControl*, QStringList>> runtimeControlsFor(
            const QStringList& cameraIds) const;

        DeviceRegistry m_registry;
        FrameRouter m_frameRouter;
        AcquisitionEngine m_acquisitionEngine;
        FrameSink m_frameSink;
        PreviewStateSink m_previewStateSink;
    };
}
