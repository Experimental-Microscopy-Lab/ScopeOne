// The ScopeOne devices of a cfg file
#include "internal/ScopeOneProvider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <QVariantMap>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

#include "internal/DriverHostClient.h"
#include "internal/DriverHostProtocol.h"
#include "internal/MicroManagerProvider.h"
#include "scopeone/DriverManifest.h"
#include "scopeone/HardwareConfiguration.h"

namespace scopeone::core::internal
{
    namespace
    {
        // A description file is a device of the Serial driver. The Serial library runs it, and the
        // file reaches the driver as the pre-init property Description File.
        const DiscoveredDriver* libraryOf(const QList<DiscoveredDriver>& drivers, const DiscoveredDriver& driver)
        {
            if (!driver.description)
            {
                return &driver;
            }
            for (const DiscoveredDriver& candidate : drivers)
            {
                if (candidate.error.isEmpty() && runsDescriptions(candidate))
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        void addDescriptionFile(const DiscoveredDriver& driver, QVariantMap& preInitProperties)
        {
            if (driver.description)
            {
                preInitProperties.insert(QStringLiteral("Description File"), driver.path);
            }
        }

        // Creates the device of one line in a DriverHost and applies the properties that are not
        // pre-init. The pre-init ones reach the driver as the arguments of createDevice.
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
            const DiscoveredDriver* library = libraryOf(drivers, *driver);
            if (!library)
            {
                *errorMessage = QStringLiteral("No driver for %1").arg(line.driverId);
                return {};
            }
            addDescriptionFile(*driver, preInitProperties);
            const QString libraryPath = library->path;
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

namespace scopeone::core
{
    DriverDescription describeDriver(const QString& driverId, const QString& deviceName)
    {
        DriverDescription description;
        const QList<DiscoveredDriver> drivers = discoverDrivers();
        const DiscoveredDriver* driver = findDriver(drivers, driverId);
        const DiscoveredDriver* library = driver ? internal::libraryOf(drivers, *driver) : nullptr;
        if (!library)
        {
            description.error = QStringLiteral("No driver %1").arg(driverId);
            return description;
        }
        QVariantMap values;
        internal::addDescriptionFile(*driver, values);
        QProcess process;
        process.start(QDir(QCoreApplication::applicationDirPath()).filePath(internal::driverhost::kExecutableFileName),
                      {QStringLiteral("--describe"), QStringLiteral("--driver"), library->path,
                       QStringLiteral("--device"), deviceName, QStringLiteral("--properties"),
                       QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(values)).toJson(QJsonDocument::Compact))});
        if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            process.kill();
            description.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
            if (description.error.isEmpty())
            {
                description.error = process.errorString();
            }
            return description;
        }
        const QJsonObject result = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        // A description file lists its devices itself, as the Serial driver has none of its own
        const QJsonArray names = driver->description ? driver->metadata.value(QStringLiteral("devices")).toArray()
                                                     : result.value(QStringLiteral("devices")).toArray();
        for (const QJsonValue& name : names)
        {
            description.deviceNames.append(name.toString());
        }
        for (const QJsonValue& value : result.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject property = value.toObject();
            QStringList allowed;
            for (const QJsonValue& allowedValue : property.value(QStringLiteral("allowedValues")).toArray())
            {
                allowed.append(allowedValue.toString());
            }
            description.properties.append({property.value(QStringLiteral("name")).toString(),
                                           property.value(QStringLiteral("defaultValue")).toString(), allowed});
        }
        return description;
    }
}
