#pragma once

#include <QList>

#include "scopeone/HardwareProvider.h"
#include "scopeone/CameraProvider.h"

namespace scopeone::core::internal
{
    class MicroManagerProvider final : public HardwareProvider, public CameraProvider
    {
    public:
        explicit MicroManagerProvider(CameraProvider* cameraProvider);

        HardwareProviderDescriptor descriptor() const override;
        QList<HardwareDeviceDescriptor> devices() const override;
        void setDevices(const QList<HardwareDeviceDescriptor>& devices);

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
                               scopeone::core::ImageFrame& frame,
                               int timeoutMs) override;

    private:
        CameraProvider* m_cameraProvider{nullptr};
        QList<HardwareDeviceDescriptor> m_devices;
    };
}
