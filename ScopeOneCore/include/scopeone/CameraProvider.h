#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "scopeone/HardwareCapabilities.h"
#include "scopeone/ImageFrame.h"

namespace scopeone::core
{
    class SCOPEONE_SDK_EXPORT CameraProvider : public DevicePropertyProvider
    {
    public:
        using FrameSink = std::function<void(const ImageFrame&)>;
        using PreviewStateSink = std::function<void(bool)>;

        ~CameraProvider() override;

        virtual void setFrameSink(FrameSink sink) = 0;
        virtual void setPreviewStateSink(PreviewStateSink sink) = 0;
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
        // Whether the camera can take a series of images in one sequence acquisition, which
        // lets the hardware time the images, for example while a stage steps through a
        // sequence of positions
        virtual bool supportsEventSequence(const QString& cameraId) const
        {
            (void)cameraId;
            return false;
        }
        // Takes count images in one sequence acquisition of the camera
        virtual bool captureEventSequence(const QString& cameraId,
                                          int count,
                                          std::vector<ImageFrame>& frames,
                                          int timeoutMs = 10000)
        {
            (void)cameraId;
            (void)count;
            (void)frames;
            (void)timeoutMs;
            return false;
        }
    };
}
