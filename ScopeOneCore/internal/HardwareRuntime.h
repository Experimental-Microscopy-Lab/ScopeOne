#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

#include <memory>

#include "scopeone/HardwareProvider.h"
#include "scopeone/ClockService.h"
#include "scopeone/CameraProvider.h"
#include "internal/AcquisitionEngine.h"
#include "internal/FrameRouter.h"

namespace scopeone::core::internal
{
    class DeviceRegistry : public QObject
    {
        Q_OBJECT

    public:
        explicit DeviceRegistry(QObject* parent = nullptr);

        void clear();
        bool registerProvider(const HardwareProviderPtr& provider);
        void unregisterProvider(const QString& providerId);
        void refreshProvider(const QString& providerId);
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
            QList<HardwareDeviceDescriptor> devices;
        };

        QHash<QString, ProviderEntry> m_providers;
    };

    class HardwareRuntime : public QObject, public CameraProvider
    {
        Q_OBJECT

    public:
        explicit HardwareRuntime(QObject* parent = nullptr);

        void setFrameSink(FrameSink sink) override;
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

        DeviceRegistry* deviceRegistry() { return &m_registry; }
        const DeviceRegistry* deviceRegistry() const { return &m_registry; }
        AcquisitionEngine* acquisitionEngine() { return &m_acquisitionEngine; }
        ClockService* clockService() { return &m_clockService; }
        FrameRouter* frameRouter() { return &m_frameRouter; }
        void clear();
        bool registerProvider(const HardwareProviderPtr& provider);
        void unregisterProvider(const QString& providerId);
        void refreshProvider(const QString& providerId);

    signals:
        void devicesChanged();

    private:
        CameraProvider* cameraProviderForDevice(const QString& logicalId) const;
        QList<CameraProvider*> cameraProviders() const;

        DeviceRegistry m_registry;
        ClockService m_clockService;
        FrameRouter m_frameRouter;
        AcquisitionEngine m_acquisitionEngine;
        FrameSink m_frameSink;
    };
}
