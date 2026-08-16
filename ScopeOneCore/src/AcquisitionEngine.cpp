#include "internal/AcquisitionEngine.h"

#include "scopeone/CameraProvider.h"
#include "internal/HardwareRuntime.h"

#include <QSet>

namespace scopeone::core::internal
{
    AcquisitionEngine::AcquisitionEngine(DeviceRegistry* deviceRegistry, QObject* parent)
        : QObject(parent)
          , m_deviceRegistry(deviceRegistry)
    {
    }

    void AcquisitionEngine::prepare()
    {
        m_state = State::Prepared;
    }

    void AcquisitionEngine::reset()
    {
        m_state = State::Idle;
    }

    bool AcquisitionEngine::start(const QString& cameraIdOrAll)
    {
        if (!m_deviceRegistry || m_state == State::Idle)
        {
            return false;
        }
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        bool started = false;
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            QList<CameraProvider*> startedProviders;
            QSet<CameraProvider*> visited;
            started = true;
            for (const HardwareDeviceDescriptor& device : m_deviceRegistry->devices())
            {
                if (device.kind != HardwareDeviceKind::Camera)
                {
                    continue;
                }
                const HardwareProviderPtr provider = m_deviceRegistry->provider(device.providerId);
                auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
                if (!cameraProvider || visited.contains(cameraProvider))
                {
                    continue;
                }
                visited.insert(cameraProvider);
                if (!cameraProvider->startPreview())
                {
                    started = false;
                    for (CameraProvider* activeProvider : startedProviders)
                    {
                        activeProvider->stopPreview();
                    }
                    break;
                }
                startedProviders.append(cameraProvider);
            }
            started = started && !startedProviders.isEmpty();
        }
        else
        {
            const HardwareProviderPtr provider = m_deviceRegistry->providerForDevice(target);
            auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
            started = cameraProvider && cameraProvider->startPreviewFor(target);
        }
        if (started)
        {
            m_state = State::Running;
        }
        return started;
    }

    bool AcquisitionEngine::stop(const QString& cameraIdOrAll)
    {
        if (!m_deviceRegistry || m_state == State::Idle)
        {
            return false;
        }
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        bool stopped = false;
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            QSet<CameraProvider*> visited;
            stopped = true;
            bool found = false;
            for (const HardwareDeviceDescriptor& device : m_deviceRegistry->devices())
            {
                if (device.kind != HardwareDeviceKind::Camera)
                {
                    continue;
                }
                const HardwareProviderPtr provider = m_deviceRegistry->provider(device.providerId);
                auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
                if (!cameraProvider || visited.contains(cameraProvider))
                {
                    continue;
                }
                found = true;
                visited.insert(cameraProvider);
                stopped = cameraProvider->stopPreview() && stopped;
            }
            stopped = found && stopped;
        }
        else
        {
            const HardwareProviderPtr provider = m_deviceRegistry->providerForDevice(target);
            auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
            stopped = cameraProvider && cameraProvider->stopPreviewFor(target);
        }
        if (stopped && target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_state = State::Prepared;
        }
        return stopped;
    }
}
