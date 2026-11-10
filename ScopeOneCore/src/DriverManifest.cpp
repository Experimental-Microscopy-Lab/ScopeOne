#include "scopeone/DriverManifest.h"

#include "scopeone/Driver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLibrary>
#include <QPluginLoader>
#include <QRegularExpression>
#include <QStandardPaths>

namespace scopeone::core
{
    QStringList driverDirectories()
    {
        return {
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("drivers")),
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("drivers"))};
    }

    namespace
    {
        // A description file has a properties object. The manifests of the libraries do not, so they
        // are skipped here.
        bool readDeviceDescription(const QString& path, DiscoveredDriver& driver)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                return false;
            }
            const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
            if (!object.value(QStringLiteral("properties")).isObject())
            {
                return false;
            }
            // The file name is the id, as the cfg names the description by it
            driver.manifest.id = QFileInfo(path).completeBaseName();
            driver.manifest.name = object.value(QStringLiteral("name")).toString();
            driver.manifest.metadata = object;
            driver.path = path;
            driver.metadata = object;
            driver.description = true;
            return true;
        }
    }

    QList<DiscoveredDriver> discoverDrivers(const QStringList& directories)
    {
        QList<DiscoveredDriver> drivers;
        const QStringList searchDirectories = directories.isEmpty() ? driverDirectories() : directories;
        for (const QString& directoryPath : searchDirectories)
        {
            const QDir directory(directoryPath);
            for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
            {
                if (file.suffix() == QStringLiteral("json"))
                {
                    DiscoveredDriver description;
                    if (readDeviceDescription(file.absoluteFilePath(), description))
                    {
                        drivers.append(std::move(description));
                    }
                    continue;
                }
                if (!QLibrary::isLibrary(file.absoluteFilePath()))
                {
                    continue;
                }
                // Reads the manifest Q_PLUGIN_METADATA put in the file, without loading the library
                QPluginLoader loader(file.absoluteFilePath());
                const QJsonObject loaderMetadata = loader.metaData();
                const QString interfaceId = loaderMetadata.value(QStringLiteral("IID")).toString();
                if (interfaceId.isEmpty())
                {
                    continue;
                }

                DiscoveredDriver driver;
                driver.path = file.absoluteFilePath();
                driver.interfaceId = interfaceId;
                driver.metadata = loaderMetadata.value(QStringLiteral("MetaData")).toObject();
                parseDriverManifest(driver.metadata, driver.manifest, &driver.error);
                if (driver.error.isEmpty() && interfaceId != QStringLiteral(ScopeOneDriver_iid))
                {
                    driver.error = QStringLiteral("library is not a ScopeOne driver");
                }
                drivers.append(std::move(driver));
            }
        }
        return drivers;
    }

    bool runsDescriptions(const DiscoveredDriver& driver)
    {
        return driver.manifest.id == QStringLiteral("scopeone.serial");
    }

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

    bool parseDriverManifest(const QJsonObject& metadata,
                             DriverManifest& manifest,
                             QString* errorMessage)
    {
        const QString id = metadata.value(QStringLiteral("id")).toString().trimmed();
        const QString name = metadata.value(QStringLiteral("name")).toString().trimmed();
        const QString version = metadata.value(QStringLiteral("version")).toString().trimmed();
        const QString kind = metadata.value(QStringLiteral("kind")).toString().trimmed();
        const int apiVersion = metadata.value(QStringLiteral("scopeOneApi")).toInt();
        const QString coreVersion = metadata.value(QStringLiteral("scopeOneCore")).toString();

        QString error;
        static const QRegularExpression idPattern(
            QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
        if (id.isEmpty() || name.isEmpty() || version.isEmpty())
        {
            error = QStringLiteral("manifest requires id, name and version");
        }
        else if (!idPattern.match(id).hasMatch())
        {
            error = QStringLiteral("id contains unsupported characters");
        }
        else if (kind != QStringLiteral("driver"))
        {
            error = QStringLiteral("kind must be 'driver'");
        }
        else if (apiVersion != ScopeOneDriverApiVersion)
        {
            error = QStringLiteral("unsupported ScopeOne API %1").arg(apiVersion);
        }
        // Drivers share C++ types with the Core, so they load only into the MAJOR.MINOR release
        // they were built against. The VERSION file changes that release when the SDK changes.
        else if (coreVersion.section(QLatin1Char('.'), 0, 1)
                 != QStringLiteral(SCOPEONE_CORE_VERSION_STRING).section(QLatin1Char('.'), 0, 1))
        {
            error = QStringLiteral("built against ScopeOne Core %1, rebuild it for %2")
                        .arg(coreVersion.isEmpty() ? QStringLiteral("unknown") : coreVersion,
                             QStringLiteral(SCOPEONE_CORE_VERSION_STRING));
        }

        if (!error.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = error;
            }
            return false;
        }

        manifest.id = id;
        manifest.name = name;
        manifest.version = version;
        manifest.metadata = metadata;
        if (errorMessage)
        {
            errorMessage->clear();
        }
        return true;
    }
}
