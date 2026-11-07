// The ScopeOne devices of a cfg file
#include "internal/ScopeOneProvider.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QVariantMap>
#include <QtConcurrent>

#include <algorithm>

#include "internal/DriverHostClient.h"
#include "internal/MicroManagerProvider.h"
#include "scopeone/DriverManifest.h"

namespace scopeone::core::internal
{
    namespace
    {
        // Creates the device of one line in a DriverHost and applies the properties that are not
        // pre-init. The pre-init ones reach the driver as the arguments of createDevice.
        DevicePtr startDevice(const ScopeOneDeviceLine& line,
                              const QList<DiscoveredDriver>& drivers,
                              QString* errorMessage)
        {
            const DiscoveredDriver* driver = nullptr;
            for (const DiscoveredDriver& candidate : drivers)
            {
                if (candidate.error.isEmpty() && candidate.manifest.id == line.driverId)
                {
                    driver = &candidate;
                    break;
                }
            }
            if (!driver)
            {
                *errorMessage = QStringLiteral("No driver %1").arg(line.driverId);
                return {};
            }

            QVariantMap preInitProperties;
            for (const auto& property : line.properties)
            {
                preInitProperties.insert(property.first, property.second);
            }
            DevicePtr device = createScopeOneDevice(
                line.label, driver->path, line.deviceName, preInitProperties, errorMessage);
            if (!device)
            {
                return {};
            }
            for (const HardwareDeviceDescriptor& peripheral : device->devices())
            {
                const QStringList names = device->listProperties(peripheral.logicalId);
                for (const auto& property : line.properties)
                {
                    if (names.contains(property.first)
                        && !device->isPropertyPreInit(peripheral.logicalId, property.first)
                        && !device->setProperty(
                            peripheral.logicalId, property.first, property.second, errorMessage))
                    {
                        return {};
                    }
                }
            }
            return device;
        }
    }

    // Reads the ScopeOneDevice, ScopeOneProperty and ScopeOneConfigGroup lines, which follow the
    // fields of Device, Property and ConfigGroup of Micro-Manager
    ScopeOneConfiguration readScopeOneConfiguration(const QString& configPath)
    {
        ScopeOneConfiguration configuration;
        QFile file(configPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return configuration;
        }

        // The properties are collected by label after the whole file is read, so their place
        // relative to the device line does not matter
        QHash<QString, QList<QPair<QString, QString>>> properties;
        QTextStream stream(&file);
        while (!stream.atEnd())
        {
            const QString line = stream.readLine().trimmed();
            if (!line.startsWith(QStringLiteral("ScopeOne")))
            {
                continue;
            }

            const QStringList device = splitConfigLine(line, 4);
            if (device.size() == 4 && device[0] == QStringLiteral("ScopeOneDevice"))
            {
                const bool used = std::any_of(configuration.devices.cbegin(), configuration.devices.cend(),
                                              [&](const ScopeOneDeviceLine& existing)
                                              { return existing.label == device[1]; });
                if (used)
                {
                    configuration.rejected.append(
                        QStringLiteral("%1 (label used twice)").arg(device[1]));
                }
                else
                {
                    configuration.devices.append({device[1], device[2], device[3], {}});
                }
                continue;
            }
            if (device.size() == 4 && device[0] == QStringLiteral("ScopeOneProperty"))
            {
                properties[device[1]].append({device[2], device[3]});
                continue;
            }
            const QStringList group = splitConfigLine(line, 6);
            if (group.size() == 6 && group[0] == QStringLiteral("ScopeOneConfigGroup"))
            {
                configuration.configGroups.append({group[1], group[2], group[3], group[4], group[5]});
            }
        }
        for (ScopeOneDeviceLine& device : configuration.devices)
        {
            device.properties = properties.take(device.label);
        }
        for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        {
            configuration.rejected.append(QStringLiteral("%1 (no ScopeOneDevice)").arg(it.key()));
        }
        return configuration;
    }

    ScopeOneProvider::LoadResult ScopeOneProvider::load(const QString& configPath)
    {
        LoadResult result;
        m_configuration = readScopeOneConfiguration(configPath);
        result.failedDevices = m_configuration.rejected;
        m_devices.clear();
        const QList<DiscoveredDriver> drivers = discoverDrivers();

        // Each DriverHost loads its device on its own, which can take seconds, so they start
        // together
        QList<QFuture<DevicePtr>> hosts;
        for (const ScopeOneDeviceLine& line : std::as_const(m_configuration.devices))
        {
            hosts.append(QtConcurrent::run([line, &drivers]()
            {
                QString error;
                DevicePtr device = startDevice(line, drivers, &error);
                if (!device)
                {
                    qWarning().noquote() << QStringLiteral("Failed to start ScopeOne device '%1': %2")
                                                .arg(line.label, error);
                }
                return device;
            }));
        }
        for (qsizetype index = 0; index < hosts.size(); ++index)
        {
            const DevicePtr device = hosts[index].result();
            if (!device)
            {
                result.failedDevices.append(m_configuration.devices[index].label);
                continue;
            }
            m_devices.append(device);
            ++result.successCount;
        }
        return result;
    }

    QList<DevicePtr> ScopeOneProvider::takeDevices()
    {
        QList<DevicePtr> taken;
        taken.swap(m_devices);
        return taken;
    }
}
