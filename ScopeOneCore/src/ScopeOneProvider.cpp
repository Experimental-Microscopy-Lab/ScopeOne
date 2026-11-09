// The ScopeOne devices of a cfg file
#include "internal/ScopeOneProvider.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QVariantMap>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

#include "internal/DriverHostClient.h"
#include "internal/MicroManagerProvider.h"
#include "scopeone/DriverManifest.h"
#include "scopeone/HardwareConfiguration.h"

namespace scopeone::core::internal
{
    namespace
    {
        // Creates the device of one line in a DriverHost and applies the properties that are not
        // pre-init. The pre-init ones reach the driver as the arguments of createDevice.
        // The driver with the id that loads, or nullptr
        const DiscoveredDriver* findDriver(const QList<DiscoveredDriver>& drivers, const QString& id)
        {
            for (const DiscoveredDriver& candidate : drivers)
            {
                if (candidate.error.isEmpty() && candidate.manifest.id == id)
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        DevicePtr startDevice(const ScopeOneDeviceLine& line,
                              const QList<DiscoveredDriver>& drivers,
                              QString* errorMessage)
        {
            const DiscoveredDriver* driver = findDriver(drivers, line.driverId);
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
            // A description file is a device of the Serial driver, which gets the file as a pre-init property
            QString libraryPath = driver->path;
            if (driver->description)
            {
                const DiscoveredDriver* serial = findDriver(drivers, QStringLiteral("scopeone.serial"));
                if (!serial)
                {
                    *errorMessage = QStringLiteral("No driver scopeone.serial for %1").arg(line.driverId);
                    return {};
                }
                libraryPath = serial->path;
                preInitProperties.insert(QStringLiteral("Description File"), driver->path);
            }
            DevicePtr device = createScopeOneDevice(
                line.label, libraryPath, line.deviceName, preInitProperties, errorMessage);
            if (!device)
            {
                return {};
            }
            for (const HardwareDeviceDescriptor& peripheral : device->devices())
            {
                const QStringList names = device->listProperties(peripheral.logicalId);
                // One read of every property fills the cache that the presets are matched against
                for (const QString& name : names)
                {
                    device->getProperty(peripheral.logicalId, name, false);
                }
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

    // Reads the ScopeOne lines of a cfg file: the devices with their properties, the ScopeOne
    // settings of the presets and the light source channels
    ScopeOneConfiguration readScopeOneConfiguration(const QString& configPath)
    {
        ScopeOneConfiguration configuration;
        const HardwareConfiguration document = HardwareConfiguration::read(configPath, nullptr);
        configuration.devices = document.scopeOneDevices();
        for (const HardwareConfiguration::Group& group : document.configGroups())
        {
            for (const HardwareConfiguration::Preset& preset : group.presets)
            {
                for (const HardwareConfiguration::PresetSetting& setting : preset.settings)
                {
                    if (setting.source == HardwareConfiguration::Source::ScopeOne)
                    {
                        configuration.configGroups.append(
                            {group.name, preset.name, setting.device, setting.property, setting.value});
                    }
                }
            }
        }
        configuration.lightSources = document.lightSources();
        configuration.rejected = document.rejectedLines();
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
        QList<QFuture<std::pair<DevicePtr, QString>>> hosts;
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
                return std::make_pair(device, error);
            }));
        }
        for (qsizetype index = 0; index < hosts.size(); ++index)
        {
            const auto [device, error] = hosts[index].result();
            if (!device)
            {
                result.failedDevices.append(
                    QStringLiteral("%1 (%2)").arg(m_configuration.devices[index].label, error));
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

    bool ScopeOneProvider::defineConfig(const QString& group, const QString& preset,
                                        const QList<ScopeOneConfigGroupLine>& settings,
                                        QString* errorMessage)
    {
        if (group.trimmed().isEmpty() || preset.trimmed().isEmpty()) { if (errorMessage) *errorMessage = QStringLiteral("Invalid config name"); return false; }
        deleteConfig(group, preset, nullptr);
        for (const auto& setting : settings) m_configuration.configGroups.append({group, preset, setting.label, setting.property, setting.value});
        return true;
    }
    bool ScopeOneProvider::deleteConfig(const QString& group, const QString& preset, QString*)
    {
        for (auto it = m_configuration.configGroups.begin(); it != m_configuration.configGroups.end(); )
            it = (it->group == group && it->preset == preset) ? m_configuration.configGroups.erase(it) : std::next(it);
        return true;
    }
    bool ScopeOneProvider::renameConfigGroup(const QString& group, const QString& newGroup, QString*)
    { for (auto& line : m_configuration.configGroups) if (line.group == group) line.group = newGroup; return true; }
    bool ScopeOneProvider::renameConfig(const QString& group, const QString& preset, const QString& newPreset, QString*)
    { for (auto& line : m_configuration.configGroups) if (line.group == group && line.preset == preset) line.preset = newPreset; return true; }
    bool ScopeOneProvider::deleteConfigGroup(const QString& group, QString*)
    { for (auto it = m_configuration.configGroups.begin(); it != m_configuration.configGroups.end(); ) it = it->group == group ? m_configuration.configGroups.erase(it) : std::next(it); return true; }
}
