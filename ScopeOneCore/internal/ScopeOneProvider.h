#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "scopeone/Device.h"
#include "scopeone/HardwareConfiguration.h"

namespace scopeone::core::internal
{
    using ScopeOneDeviceLine = HardwareConfiguration::ScopeOneDevice;
    using LightSourceLine = HardwareConfiguration::LightSource;

    struct ScopeOneConfigGroupLine
    {
        QString group;
        QString preset;
        QString label;
        QString property;
        QString value;
    };

    // The ScopeOne lines of a cfg file. The other lines belong to Micro-Manager.
    struct ScopeOneConfiguration
    {
        QList<ScopeOneDeviceLine> devices;
        QList<ScopeOneConfigGroupLine> configGroups;
        // In the order of the first line of each name
        QList<LightSourceLine> lightSources;
        // Lines that name no device to start, such as a second device of a label or a property of
        // a label without device, each as "label (reason)"
        QStringList rejected;
    };

    ScopeOneConfiguration readScopeOneConfiguration(const QString& configPath);

    // The devices of the loaded cfg file that are not Micro-Manager's. It is the counterpart of
    // MicroManagerProvider: each ScopeOneDevice line runs its device of a driver in a DriverHost of
    // its own, and the runtime registers each of them under the label of the line.
    class ScopeOneProvider
    {
    public:
        struct LoadResult
        {
            QStringList failedDevices;
            int successCount{0};
        };

        // Starts the devices of the file together and blocks until all have started or failed
        LoadResult load(const QString& configPath);
        const ScopeOneConfiguration& configuration() const { return m_configuration; }
        void setLightSources(const QList<LightSourceLine>& sources) { m_configuration.lightSources = sources; }
        bool defineConfig(const QString& group, const QString& preset,
                          const QList<ScopeOneConfigGroupLine>& settings,
                          QString* errorMessage = nullptr);
        bool deleteConfig(const QString& group, const QString& preset,
                          QString* errorMessage = nullptr);
        bool renameConfigGroup(const QString& group, const QString& newGroup,
                               QString* errorMessage = nullptr);
        bool renameConfig(const QString& group, const QString& preset, const QString& newPreset,
                          QString* errorMessage = nullptr);
        bool deleteConfigGroup(const QString& group, QString* errorMessage = nullptr);
        QList<DevicePtr> devices() const { return m_devices; }
        // Hands over the devices, so they can be stopped away from the calling thread
        QList<DevicePtr> takeDevices();

    private:
        ScopeOneConfiguration m_configuration;
        QList<DevicePtr> m_devices;
    };
}
