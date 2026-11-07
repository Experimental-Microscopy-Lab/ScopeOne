#pragma once

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
    inline constexpr int ScopeOneDriverApiVersion = 3;

    // The drivers directory beside the application and in the user data directory
    SCOPEONE_CORE_EXPORT QStringList driverDirectories();
    SCOPEONE_CORE_EXPORT QList<DiscoveredDriver> discoverDrivers(const QStringList& directories = {});
    SCOPEONE_CORE_EXPORT bool parseDriverManifest(const QJsonObject& metadata,
                                                  DriverManifest& manifest,
                                                  QString* errorMessage = nullptr);
}
