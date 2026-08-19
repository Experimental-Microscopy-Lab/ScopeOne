#pragma once

#include <QString>
#include <QStringList>

#include <functional>

#include "scopeone/HardwareCapabilities.h"
#include "scopeone/ImageFrame.h"

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT CameraProvider : public DevicePropertyProvider
    {
    public:
        using FrameSink = std::function<void(const ImageFrame&)>;

        ~CameraProvider() override;

        virtual void setFrameSink(FrameSink sink) = 0;
        virtual bool startPreview() = 0;
        virtual bool stopPreview() = 0;
        virtual bool startPreviewFor(const QString& cameraId) = 0;
        virtual bool stopPreviewFor(const QString& cameraId) = 0;
        virtual bool isPreviewRunning(const QString& cameraId) const = 0;
        virtual bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const = 0;
        virtual bool setExposure(const QString& cameraIdOrAll, double exposureMs) = 0;
        virtual bool setROI(const QString& cameraId, int x, int y, int width, int height) = 0;
        virtual bool clearROI(const QString& cameraId) = 0;
        virtual bool getROI(const QString& cameraId,
                            int& x,
                            int& y,
                            int& width,
                            int& height) = 0;
        virtual bool captureEventFrame(const QString& cameraId,
                                       ImageFrame& frame,
                                       int timeoutMs = 1500) = 0;
    };
}
