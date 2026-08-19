#include "internal/CameraManager.h"

#include <QTimer>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        QString normalizedCameraId(const QString& cameraId)
        {
            return cameraId.trimmed();
        }

        QString propertyCacheKey(const QString& cameraId, const QString& name)
        {
            return QStringLiteral("%1:%2").arg(cameraId, name);
        }
    }

    CameraManager::CameraManager(QObject* parent)
        : QObject(parent)
    {
        qRegisterMetaType<scopeone::core::ImageFrame>("scopeone::core::ImageFrame");
    }

    CameraManager::~CameraManager()
    {
        shutdownNow();
    }

    void CameraManager::setFrameSink(FrameSink sink)
    {
        m_frameSink = std::move(sink);
    }

    // Selects one camera backend and forwards its runtime signals
    bool CameraManager::activateBackend(CameraBackend::Kind kind)
    {
        if (m_backend && m_backend->kind() == kind)
        {
            return true;
        }

        shutdownNow();
        m_backend = kind == CameraBackend::Kind::Native
                        ? createNativeCameraBackend(m_processingFrameGate)
                        : createDriverHostCameraBackend(m_processingFrameGate);
        if (!m_backend)
        {
            return false;
        }

        connect(m_backend.get(), &CameraBackend::rawFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    if (m_frameSink)
                    {
                        m_frameSink(frame);
                    }
                });
        // Keeps processing input on the producer thread
        connect(m_backend.get(), &CameraBackend::processingFrameReady,
                this, &CameraManager::processingFrameReady,
                Qt::DirectConnection);
        connect(m_backend.get(), &CameraBackend::rawFramesAcquired,
                this, &CameraManager::rawFramesAcquired);
        connect(m_backend.get(), &CameraBackend::recordingFramesReady,
                this, &CameraManager::recordingFramesReady);
        connect(m_backend.get(), &CameraBackend::frameDeliveryFailed,
                this, &CameraManager::frameDeliveryFailed);
        connect(m_backend.get(), &CameraBackend::previewStateChanged,
                this, &CameraManager::previewStateChanged);
        connect(m_backend.get(), &CameraBackend::driverHostControlServerListening,
                this, &CameraManager::driverHostControlServerListening);
        if (!m_backend->setHighRateFrameDeliveryEnabled(m_highRateFrameDeliveryEnabled)
            || !m_backend->setRecordingFrameDeliveryEnabled(m_recordingFrameDeliveryEnabled))
        {
            m_backend.reset();
            return false;
        }
        return true;
    }

    // Configures the in process backend for one MMCore camera
    bool CameraManager::configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                              const QString& cameraId,
                                              double exposureMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const bool configured = core
            && !normalizedId.isEmpty()
            && activateBackend(CameraBackend::Kind::Native)
            && m_backend->configureNativeCamera(core, normalizedId, exposureMs);
        if (configured)
        {
            m_propertyNamesCache.remove(normalizedId);
            m_propertyDetailsCache.clear();
        }
        return configured;
    }

    // Add one process isolated camera to the DriverHost backend
    bool CameraManager::addDriverHostCamera(const QString& cameraId,
                                       const QString& adapter,
                                       const QString& device,
                                       const QStringList& preInitProperties,
                                       const QStringList& properties,
                                       double exposureMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const bool configured = !normalizedId.isEmpty()
            && activateBackend(CameraBackend::Kind::DriverHost)
            && m_backend->addDriverHostCamera(normalizedId,
                                         adapter,
                                         device,
                                         preInitProperties,
                                         properties,
                                         exposureMs);
        if (configured)
        {
            m_propertyNamesCache.remove(normalizedId);
            m_propertyDetailsCache.clear();
        }
        return configured;
    }

    // Tears down the active backend synchronously
    void CameraManager::shutdownNow()
    {
        m_processingFrameGate.setEnabled(false);
        m_backend.reset();
        m_recordingFrameDeliveryEnabled = false;
        m_highRateFrameDeliveryEnabled = false;
        m_propertyNamesCache.clear();
        m_propertyDetailsCache.clear();
    }

    // Releases the active backend after its asynchronous teardown completes
    void CameraManager::shutdown(std::function<void(const QString&)> completion)
    {
        m_processingFrameGate.setEnabled(false);
        if (!m_backend)
        {
            m_recordingFrameDeliveryEnabled = false;
            m_highRateFrameDeliveryEnabled = false;
            m_propertyNamesCache.clear();
            m_propertyDetailsCache.clear();
            if (completion)
            {
                completion({});
            }
            return;
        }

        CameraBackend* const backend = m_backend.get();
        backend->shutdown([this, backend, completion = std::move(completion)](
                                   const QString& errorMessage) mutable
        {
            QTimer::singleShot(0, this, [this,
                                        backend,
                                        completion = std::move(completion),
                                        errorMessage]() mutable
            {
                if (errorMessage.isEmpty() && m_backend.get() == backend)
                {
                    m_backend.reset();
                }
                if (errorMessage.isEmpty())
                {
                    m_recordingFrameDeliveryEnabled = false;
                    m_highRateFrameDeliveryEnabled = false;
                    m_propertyNamesCache.clear();
                    m_propertyDetailsCache.clear();
                }
                if (completion)
                {
                    completion(errorMessage);
                }
            });
        });
    }

    bool CameraManager::startPreview()
    {
        return m_backend && m_backend->startPreview();
    }

    bool CameraManager::stopPreview()
    {
        return m_backend && m_backend->stopPreview();
    }

    // Report whether cameras are isolated in DriverHost processes
    bool CameraManager::usesDriverHostBackend() const
    {
        return m_backend && m_backend->kind() == CameraBackend::Kind::DriverHost;
    }

    bool CameraManager::startPreviewFor(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_backend && m_backend->startPreviewFor(normalizedId);
    }

    bool CameraManager::stopPreviewFor(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_backend && m_backend->stopPreviewFor(normalizedId);
    }

    bool CameraManager::isPreviewRunning(const QString& cameraId) const
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_backend && m_backend->isPreviewRunning(normalizedId);
    }

    void CameraManager::setFrameDeliveryPaused(bool paused)
    {
        if (m_backend)
        {
            m_backend->setFrameDeliveryPaused(paused);
        }
    }

    // Switches the active backend between preview and lossless recording delivery
    bool CameraManager::setRecordingFrameDeliveryEnabled(bool enabled)
    {
        const bool ok = !m_backend || m_backend->setRecordingFrameDeliveryEnabled(enabled);
        if (ok || !enabled)
        {
            m_recordingFrameDeliveryEnabled = enabled;
        }
        return ok;
    }

    // Switches processing delivery independently of preview display rate
    bool CameraManager::setHighRateFrameDeliveryEnabled(bool enabled)
    {
        const bool ok = !m_backend || m_backend->setHighRateFrameDeliveryEnabled(enabled);
        if (ok)
        {
            m_highRateFrameDeliveryEnabled = enabled;
        }
        return ok;
    }

    // Checks whether a completed processing task still owns its permit
    bool CameraManager::isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token)
    {
        return m_processingFrameGate.isCurrent(cameraId, token);
    }

    // Releases the permit held by one completed processing task
    void CameraManager::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        m_processingFrameGate.release(cameraId, token);
    }

    bool CameraManager::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        exposureMs = 0.0;
        const QString target = cameraIdOrAll.trimmed();
        return !target.isEmpty() && m_backend && m_backend->getExposure(target, exposureMs);
    }

    bool CameraManager::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QString target = cameraIdOrAll.trimmed();
        const bool ok = !target.isEmpty() && m_backend && m_backend->setExposure(target, exposureMs);
        if (ok)
        {
            m_propertyDetailsCache.clear();
        }
        return ok;
    }

    QStringList CameraManager::listProperties(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (normalizedId.isEmpty() || !m_backend)
        {
            return {};
        }
        const auto cached = m_propertyNamesCache.constFind(normalizedId);
        if (cached != m_propertyNamesCache.constEnd())
        {
            return cached.value();
        }
        const QStringList names = m_backend->listProperties(normalizedId);
        if (!names.isEmpty())
        {
            m_propertyNamesCache.insert(normalizedId, names);
        }
        return names;
    }

    QString CameraManager::getProperty(const QString& cameraId,
                                       const QString& name,
                                       bool fromCache)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (fromCache)
        {
            const auto cached = m_propertyDetailsCache.constFind(cacheKey);
            if (cached != m_propertyDetailsCache.constEnd())
            {
                return cached.value().value;
            }
        }
        CameraPropertyReadback readback;
        if (normalizedId.isEmpty()
            || !m_backend
            || !m_backend->readPropertyDetails(normalizedId, name, fromCache, readback))
        {
            m_propertyDetailsCache.remove(cacheKey);
            return {};
        }

        m_propertyDetailsCache.insert(cacheKey, readback);
        return readback.value;
    }

    bool CameraManager::setProperty(const QString& cameraId,
                                    const QString& name,
                                    const QString& value,
                                    QString* errorMessage)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        if (normalizedId.isEmpty() || !m_backend)
        {
            return false;
        }
        const bool ok = m_backend->setProperty(normalizedId, name, value, errorMessage);
        if (ok)
        {
            m_propertyDetailsCache.clear();
        }
        return ok;
    }

    QString CameraManager::getPropertyType(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).type;
    }

    bool CameraManager::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).readOnly;
    }

    bool CameraManager::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).preInit;
    }

    QStringList CameraManager::getAllowedPropertyValues(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).allowedValues;
    }

    bool CameraManager::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).hasLimits;
    }

    double CameraManager::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).lowerLimit;
    }

    double CameraManager::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const QString cacheKey = propertyCacheKey(normalizedId, name);
        if (!m_propertyDetailsCache.contains(cacheKey))
        {
            getProperty(normalizedId, name);
        }
        return m_propertyDetailsCache.value(cacheKey).upperLimit;
    }

    bool CameraManager::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const bool ok = !normalizedId.isEmpty()
            && m_backend
            && m_backend->setROI(normalizedId, x, y, width, height);
        if (ok)
        {
            m_propertyDetailsCache.clear();
        }
        return ok;
    }

    bool CameraManager::clearROI(const QString& cameraId)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        const bool ok = !normalizedId.isEmpty() && m_backend && m_backend->clearROI(normalizedId);
        if (ok)
        {
            m_propertyDetailsCache.clear();
        }
        return ok;
    }

    bool CameraManager::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty() && m_backend && m_backend->getROI(normalizedId, x, y, width, height);
    }

    bool CameraManager::captureEventFrame(const QString& cameraId, ImageFrame& frame, int timeoutMs)
    {
        const QString normalizedId = normalizedCameraId(cameraId);
        return !normalizedId.isEmpty()
            && m_backend
            && m_backend->captureEventFrame(normalizedId, frame, timeoutMs);
    }

}
