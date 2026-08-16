#pragma once

#include <QString>
#include <QStringList>

#include <functional>

#include "scopeone/ImageFrame.h"

namespace scopeone::core
{
    class CameraProvider
    {
    public:
        using FrameSink = std::function<void(const ImageFrame&)>;

        virtual ~CameraProvider() = default;

        virtual void setFrameSink(FrameSink sink) = 0;
        virtual bool startPreview() = 0;
        virtual bool stopPreview() = 0;
        virtual bool startPreviewFor(const QString& cameraId) = 0;
        virtual bool stopPreviewFor(const QString& cameraId) = 0;
        virtual bool isPreviewRunning(const QString& cameraId) const = 0;
        virtual bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const = 0;
        virtual bool setExposure(const QString& cameraIdOrAll, double exposureMs) = 0;
        virtual QStringList listProperties(const QString& cameraId) = 0;
        virtual QString getProperty(const QString& cameraId,
                                    const QString& name,
                                    bool fromCache = false) = 0;
        virtual bool setProperty(const QString& cameraId,
                                 const QString& name,
                                 const QString& value,
                                 QString* errorMessage = nullptr) = 0;
        virtual QString getPropertyType(const QString& cameraId, const QString& name) = 0;
        virtual bool isPropertyReadOnly(const QString& cameraId, const QString& name) = 0;
        virtual bool isPropertyPreInit(const QString& cameraId, const QString& name) = 0;
        virtual QStringList getAllowedPropertyValues(const QString& cameraId,
                                                     const QString& name) = 0;
        virtual bool hasPropertyLimits(const QString& cameraId, const QString& name) = 0;
        virtual double getPropertyLowerLimit(const QString& cameraId, const QString& name) = 0;
        virtual double getPropertyUpperLimit(const QString& cameraId, const QString& name) = 0;
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
