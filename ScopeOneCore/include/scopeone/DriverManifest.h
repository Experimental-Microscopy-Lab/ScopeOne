#pragma once

#include "scopeone/Driver.h"
#include "scopeone/scopeone_core_export.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace scopeone::core
{
    // The manifest of a driver, which ScopeOne reads from the library without loading it and
    // ScopeOne_DriverHost loads for its device
    struct DriverManifest
    {
        QString id;
        QString name;
        QString version;
        QJsonObject metadata;
    };

    // A driver library, or a description file of a serial device, which the Serial driver runs.
    // For a description the path is the file itself.
    struct DiscoveredDriver
    {
        DriverManifest manifest;
        QString path;
        QString interfaceId;
        QJsonObject metadata;
        QString error;
        bool description{false};
    };

    // Raised whenever an interface of the driver SDK changes its virtual functions. A driver built
    // against other ones calls the wrong functions and crashes DriverHost, so it is rejected.
    inline constexpr int ScopeOneDriverApiVersion = 4;

    // The drivers directory beside the application and in the user data directory
    SCOPEONE_CORE_EXPORT QStringList driverDirectories();
    SCOPEONE_CORE_EXPORT QList<DiscoveredDriver> discoverDrivers(const QStringList& directories = {});
    // The Serial driver, which runs the description files and has no devices of its own
    SCOPEONE_CORE_EXPORT bool runsDescriptions(const DiscoveredDriver& driver);
    // The driver with the id that loads, or nullptr
    SCOPEONE_CORE_EXPORT const DiscoveredDriver* findDriver(const QList<DiscoveredDriver>& drivers,
                                                            const QString& id);

    // The device names of a driver and the pre-init properties of one of its devices, or the reason
    // they could not be read
    struct DriverDescription
    {
        QStringList deviceNames;
        QList<PreInitProperty> properties;
        QString error;
    };

    // Asks a DriverHost started with --describe, and blocks until it exits
    SCOPEONE_CORE_EXPORT DriverDescription describeDriver(const QString& driverId, const QString& deviceName);
    SCOPEONE_CORE_EXPORT bool parseDriverManifest(const QJsonObject& metadata,
                                                  DriverManifest& manifest,
                                                  QString* errorMessage = nullptr);
}
