#include "internal/MicroManagerProvider.h"

#include <utility>

namespace scopeone::core::internal
{
    MicroManagerProvider::MicroManagerProvider(CameraProvider* cameraProvider)
        : m_cameraProvider(cameraProvider)
    {
    }

    HardwareProviderDescriptor MicroManagerProvider::descriptor() const
    {
        return {
            QStringLiteral("micro-manager"),
            QStringLiteral("Micro-Manager"),
            QStringLiteral("MMCore")
        };
    }

    QList<HardwareDeviceDescriptor> MicroManagerProvider::devices() const
    {
        return m_devices;
    }

    void MicroManagerProvider::setDevices(const QList<HardwareDeviceDescriptor>& devices)
    {
        m_devices = devices;
    }

    void MicroManagerProvider::setFrameSink(FrameSink sink)
    {
        if (m_cameraProvider)
        {
            m_cameraProvider->setFrameSink(std::move(sink));
        }
    }

    bool MicroManagerProvider::startPreview()
    {
        return m_cameraProvider && m_cameraProvider->startPreview();
    }

    bool MicroManagerProvider::stopPreview()
    {
        return m_cameraProvider && m_cameraProvider->stopPreview();
    }

    bool MicroManagerProvider::startPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->startPreviewFor(cameraId);
    }

    bool MicroManagerProvider::stopPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->stopPreviewFor(cameraId);
    }

    bool MicroManagerProvider::isPreviewRunning(const QString& cameraId) const
    {
        return m_cameraProvider && m_cameraProvider->isPreviewRunning(cameraId);
    }

    bool MicroManagerProvider::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        return m_cameraProvider && m_cameraProvider->getExposure(cameraIdOrAll, exposureMs);
    }

    bool MicroManagerProvider::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        return m_cameraProvider && m_cameraProvider->setExposure(cameraIdOrAll, exposureMs);
    }

    QStringList MicroManagerProvider::listProperties(const QString& cameraId)
    {
        return m_cameraProvider ? m_cameraProvider->listProperties(cameraId) : QStringList{};
    }

    QString MicroManagerProvider::getProperty(const QString& cameraId,
                                              const QString& name,
                                              bool fromCache)
    {
        return m_cameraProvider ? m_cameraProvider->getProperty(cameraId, name, fromCache) : QString{};
    }

    bool MicroManagerProvider::setProperty(const QString& cameraId,
                                           const QString& name,
                                           const QString& value,
                                           QString* errorMessage)
    {
        return m_cameraProvider
            && m_cameraProvider->setProperty(cameraId, name, value, errorMessage);
    }

    QString MicroManagerProvider::getPropertyType(const QString& cameraId, const QString& name)
    {
        return m_cameraProvider
                   ? m_cameraProvider->getPropertyType(cameraId, name)
                   : QStringLiteral("Unknown");
    }

    bool MicroManagerProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        return !m_cameraProvider || m_cameraProvider->isPropertyReadOnly(cameraId, name);
    }

    bool MicroManagerProvider::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        return m_cameraProvider && m_cameraProvider->isPropertyPreInit(cameraId, name);
    }

    QStringList MicroManagerProvider::getAllowedPropertyValues(const QString& cameraId,
                                                               const QString& name)
    {
        return m_cameraProvider
                   ? m_cameraProvider->getAllowedPropertyValues(cameraId, name)
                   : QStringList{};
    }

    bool MicroManagerProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        return m_cameraProvider && m_cameraProvider->hasPropertyLimits(cameraId, name);
    }

    double MicroManagerProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        return m_cameraProvider ? m_cameraProvider->getPropertyLowerLimit(cameraId, name) : 0.0;
    }

    double MicroManagerProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        return m_cameraProvider ? m_cameraProvider->getPropertyUpperLimit(cameraId, name) : 0.0;
    }

    bool MicroManagerProvider::setROI(const QString& cameraId,
                                      int x,
                                      int y,
                                      int width,
                                      int height)
    {
        return m_cameraProvider && m_cameraProvider->setROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::clearROI(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->clearROI(cameraId);
    }

    bool MicroManagerProvider::getROI(const QString& cameraId,
                                      int& x,
                                      int& y,
                                      int& width,
                                      int& height)
    {
        return m_cameraProvider && m_cameraProvider->getROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::captureEventFrame(const QString& cameraId,
                                                 scopeone::core::ImageFrame& frame,
                                                 int timeoutMs)
    {
        return m_cameraProvider && m_cameraProvider->captureEventFrame(cameraId, frame, timeoutMs);
    }
}
