#include "internal/DaqDeviceManager.h"

#include "internal/HardwareRuntime.h"

#include <QDebug>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        void expandRasterScan(const DaqRasterScanConfig& scan,
                              DaqSessionConfig& session)
        {
            const quint64 frameLines = static_cast<quint64>(scan.activeLines)
                + static_cast<quint64>(scan.flybackLines);
            const QString name = scan.name.trimmed().isEmpty()
                                     ? QStringLiteral("Raster scan")
                                     : scan.name.trimmed();

            if (!scan.lineOutputTerminal.trimmed().isEmpty())
            {
                session.routes.append({scan.lineClock,
                                       scan.lineOutputTerminal,
                                       false});
            }

            DaqPulseTaskConfig framePulse;
            framePulse.name = name + QStringLiteral(" frame clock");
            framePulse.counter = scan.frameCounter;
            framePulse.outputTerminal = scan.frameOutputTerminal;
            framePulse.timebaseSource = scan.lineClock;
            framePulse.lowTicks = static_cast<quint32>(frameLines - 2);
            framePulse.highTicks = 2;
            session.pulseTasks.append(framePulse);

            DaqAnalogTaskConfig slowAxis;
            slowAxis.name = name + QStringLiteral(" slow axis");
            slowAxis.direction = DaqTaskDirection::Output;
            slowAxis.channels = {scan.yChannel};
            slowAxis.minimumVolts = std::min(scan.yStartVolts, scan.yEndVolts);
            slowAxis.maximumVolts = std::max(scan.yStartVolts, scan.yEndVolts);
            slowAxis.timing.sampleClock = scan.lineClock;
            slowAxis.timing.sampleRateHz = scan.nominalLineRateHz;
            slowAxis.timing.sampleEdge = DaqEdge::Falling;
            slowAxis.timing.sampleMode = DaqSampleMode::Continuous;
            slowAxis.timing.samplesPerChannel = frameLines;
            slowAxis.timing.startTrigger = scan.frameOutputTerminal;
            slowAxis.outputSamplesByScan.reserve(static_cast<qsizetype>(frameLines));

            for (quint32 line = 0; line < scan.activeLines; ++line)
            {
                const double t = static_cast<double>(line)
                    / static_cast<double>(scan.activeLines - 1);
                slowAxis.outputSamplesByScan.append(
                    scan.yStartVolts + (scan.yEndVolts - scan.yStartVolts) * t);
            }
            for (quint32 line = 0; line < scan.flybackLines; ++line)
            {
                const double t = static_cast<double>(line + 1)
                    / static_cast<double>(scan.flybackLines);
                const double smooth = t * t * (3.0 - 2.0 * t);
                slowAxis.outputSamplesByScan.append(
                    scan.yEndVolts + (scan.yStartVolts - scan.yEndVolts) * smooth);
            }
            session.analogTasks.append(std::move(slowAxis));
        }
    }

    DaqDeviceManager::DaqDeviceManager(DeviceRegistry* registry, QObject* parent)
        : QObject(parent)
          , m_registry(registry)
    {
        connect(m_registry, &DeviceRegistry::changed, this, &DaqDeviceManager::refreshProviderDevices);
        refreshProviderDevices();
    }

    DaqDeviceManager::~DaqDeviceManager()
    {
        for (const HardwareProviderPtr& provider : std::as_const(m_attachedProviders))
        {
            detachProvider(provider);
        }
    }

    QList<DaqDeviceDescriptor> DaqDeviceManager::devices() const
    {
        QList<DaqDeviceDescriptor> result = m_descriptors.values();
        std::sort(result.begin(), result.end(),
                  [](const DaqDeviceDescriptor& left,
                     const DaqDeviceDescriptor& right)
                  {
                      return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
                  });
        return result;
    }

    bool DaqDeviceManager::start(const DaqSessionConfig& config,
                                 QString* errorMessage)
    {
        const QString deviceId = config.deviceId.trimmed();
        const HardwareProviderPtr provider = m_providerByDevice.value(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown DAQ device: %1").arg(deviceId);
            }
            return false;
        }
        DaqSessionConfig expanded = config;
        expanded.deviceId = deviceId;
        expanded.rasterScans.clear();
        for (const DaqRasterScanConfig& scan : config.rasterScans)
        {
            expandRasterScan(scan, expanded);
        }
        return dynamic_cast<DaqProvider*>(provider.get())->startDaq(expanded, errorMessage);
    }

    void DaqDeviceManager::stop(const QString& deviceId)
    {
        const QString id = deviceId.trimmed();
        if (const HardwareProviderPtr provider = m_providerByDevice.value(id))
        {
            dynamic_cast<DaqProvider*>(provider.get())->stopDaq(id);
        }
    }

    DaqState DaqDeviceManager::state(const QString& deviceId) const
    {
        return m_states.value(deviceId.trimmed(), DaqState::Idle);
    }

    QString DaqDeviceManager::stateMessage(const QString& deviceId) const
    {
        return m_messages.value(deviceId.trimmed(), QStringLiteral("DAQ device is idle"));
    }

    // Follows the drivers as they are registered and removed, like the signal sources
    void DaqDeviceManager::refreshProviderDevices()
    {
        QHash<QString, HardwareProviderPtr> current;
        for (const HardwareProviderDescriptor& descriptor : m_registry->providers())
        {
            const HardwareProviderPtr provider = m_registry->provider(descriptor.id);
            if (dynamic_cast<DaqProvider*>(provider.get()))
            {
                current.insert(descriptor.id, provider);
            }
        }
        bool changed = false;
        for (auto it = m_attachedProviders.begin(); it != m_attachedProviders.end();)
        {
            if (current.value(it.key()) == it.value())
            {
                ++it;
                continue;
            }
            detachProvider(it.value());
            it = m_attachedProviders.erase(it);
            changed = true;
        }
        for (auto it = current.cbegin(); it != current.cend(); ++it)
        {
            if (m_attachedProviders.contains(it.key()))
            {
                continue;
            }
            auto* const daqProvider = dynamic_cast<DaqProvider*>(it.value().get());
            const QPointer<DaqDeviceManager> self(this);
            daqProvider->setDaqSinks(
                [self](const DaqInputChunk& chunk)
                {
                    QMetaObject::invokeMethod(self, [self, chunk]() { if (self) emit self->inputDataReady(chunk); },
                                              Qt::QueuedConnection);
                },
                [self](const QString& deviceId, DaqState state, const QString& message)
                {
                    QMetaObject::invokeMethod(self, [self, deviceId, state, message]()
                    {
                        if (self) self->updateState(deviceId, state, message);
                    }, Qt::QueuedConnection);
                });
            for (const DaqDeviceDescriptor& descriptor : daqProvider->daqDevices())
            {
                const QString deviceId = descriptor.id.trimmed();
                if (deviceId.isEmpty() || m_descriptors.contains(deviceId))
                {
                    qWarning().noquote() << QStringLiteral("Hardware provider %1: invalid or duplicate DAQ device %2")
                                                .arg(it.key(), descriptor.id);
                    continue;
                }
                m_descriptors.insert(deviceId, descriptor);
                m_providerByDevice.insert(deviceId, it.value());
                m_states.insert(deviceId, DaqState::Idle);
                m_messages.insert(deviceId, QStringLiteral("DAQ device is idle"));
            }
            m_attachedProviders.insert(it.key(), it.value());
            changed = true;
        }
        if (changed)
        {
            emit devicesChanged();
        }
    }

    void DaqDeviceManager::detachProvider(const HardwareProviderPtr& provider)
    {
        dynamic_cast<DaqProvider*>(provider.get())->setDaqSinks({}, {});
        for (auto it = m_providerByDevice.begin(); it != m_providerByDevice.end();)
        {
            if (it.value() != provider)
            {
                ++it;
                continue;
            }
            m_descriptors.remove(it.key());
            m_states.remove(it.key());
            m_messages.remove(it.key());
            it = m_providerByDevice.erase(it);
        }
    }

    void DaqDeviceManager::updateState(const QString& deviceId, DaqState state, const QString& message)
    {
        if (!m_providerByDevice.contains(deviceId))
        {
            return;
        }
        m_states.insert(deviceId, state);
        m_messages.insert(deviceId, message);
        emit stateChanged(deviceId, state, message);
        if (state == DaqState::Error)
        {
            emit deviceError(deviceId, message);
        }
    }
}
