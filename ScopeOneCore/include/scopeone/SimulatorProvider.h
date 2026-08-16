#pragma once

#include <QObject>
#include <QRect>
#include <QTimer>

#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareProvider.h"

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT SimulatorProvider final
        : public QObject,
          public HardwareProvider,
          public CameraProvider
    {
    public:
        explicit SimulatorProvider(const QString& logicalCameraId = QStringLiteral("camera.simulator"),
                                   int width = 512,
                                   int height = 512);

        HardwareProviderDescriptor descriptor() const override;
        QList<HardwareDeviceDescriptor> devices() const override;
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

    private:
        bool accepts(const QString& cameraIdOrAll) const;
        ImageFrame makeFrame();
        void updateTimerInterval();

        QString m_providerId;
        QString m_cameraId;
        int m_sensorWidth{512};
        int m_sensorHeight{512};
        QRect m_roi;
        double m_exposureMs{10.0};
        quint64 m_frameIndex{0};
        FrameSink m_frameSink;
        QTimer m_timer;
    };
}
