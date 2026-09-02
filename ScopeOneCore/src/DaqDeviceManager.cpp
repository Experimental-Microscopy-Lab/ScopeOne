#include "internal/DaqDeviceManager.h"
#include "scopeone/PluginManifest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace scopeone::core
{
    DaqController::DaqController(QObject* parent)
        : QObject(parent)
    {
    }

    DaqController::~DaqController() = default;
}

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

    DaqDeviceManager::DaqDeviceManager(QObject* parent)
        : QObject(parent)
    {
        loadPlugins();
    }

    DaqDeviceManager::~DaqDeviceManager()
    {
        for (const QPointer<DaqController>& controller : std::as_const(m_controllers))
        {
            delete controller.data();
        }
        m_controllers.clear();
        m_loaders.clear();
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
        if (!m_descriptors.contains(deviceId))
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
        DaqController* deviceController = controller(deviceId, errorMessage);
        return deviceController && deviceController->start(expanded, errorMessage);
    }

    void DaqDeviceManager::stop(const QString& deviceId)
    {
        if (DaqController* deviceController = m_controllers.value(deviceId.trimmed()))
        {
            deviceController->stop();
        }
    }

    DaqState DaqDeviceManager::state(const QString& deviceId) const
    {
        const QString id = deviceId.trimmed();
        const QPointer<DaqController> controller = m_controllers.value(id);
        return controller ? controller->state() : m_states.value(id, DaqState::Idle);
    }

    QString DaqDeviceManager::stateMessage(const QString& deviceId) const
    {
        const QString id = deviceId.trimmed();
        const QPointer<DaqController> controller = m_controllers.value(id);
        return controller ? controller->stateMessage()
                          : m_messages.value(id, QStringLiteral("DAQ device is idle"));
    }

    void DaqDeviceManager::loadPlugins()
    {
        const QStringList directories = {
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("plugins/hardware")),
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("plugins/hardware"))};

        for (const QString& path : directories)
        {
            const QDir directory(path);
            for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
            {
                if (!QLibrary::isLibrary(file.absoluteFilePath()))
                {
                    continue;
                }
                auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
                PluginManifest manifest;
                QString manifestError;
                if (!parsePluginManifest(
                        loader->metaData().value(QStringLiteral("MetaData")).toObject(),
                        PluginKind::Hardware,
                        manifest,
                        &manifestError))
                {
                    qWarning().noquote()
                        << QStringLiteral("Failed to load DAQ plugin %1: %2")
                               .arg(file.fileName(), manifestError);
                    continue;
                }
                auto* plugin = qobject_cast<DaqDevicePlugin*>(loader->instance());
                if (!plugin)
                {
                    qWarning().noquote()
                        << QStringLiteral("Failed to load DAQ plugin %1: %2")
                               .arg(file.fileName(), loader->errorString());
                    continue;
                }

                for (const DaqDeviceDescriptor& descriptor : plugin->devices())
                {
                    const QString deviceId = descriptor.id.trimmed();
                    if (deviceId.isEmpty() || m_descriptors.contains(deviceId))
                    {
                        continue;
                    }
                    m_descriptors.insert(deviceId, descriptor);
                    m_plugins.insert(deviceId, plugin);
                    m_states.insert(deviceId, DaqState::Idle);
                    m_messages.insert(deviceId, QStringLiteral("DAQ device is idle"));
                }
                m_loaders.push_back(std::move(loader));
            }
        }
    }

    DaqController* DaqDeviceManager::controller(const QString& deviceId,
                                                QString* errorMessage)
    {
        const QString id = deviceId.trimmed();
        if (DaqController* existing = m_controllers.value(id))
        {
            return existing;
        }
        DaqDevicePlugin* plugin = m_plugins.value(id);
        if (!plugin)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown DAQ device: %1").arg(id);
            }
            return nullptr;
        }
        DaqController* created = plugin->createController(id, this);
        if (!created)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to create DAQ controller: %1").arg(id);
            }
            return nullptr;
        }

        connect(created, &DaqController::stateChanged,
                this, [this, id](DaqState state, const QString& message)
                {
                    m_states.insert(id, state);
                    m_messages.insert(id, message);
                    emit stateChanged(id, state, message);
                });
        connect(created, &DaqController::controllerError,
                this, [this, id](const QString& message)
                {
                    emit deviceError(id, message);
                });
        connect(created, &DaqController::inputDataReady,
                this, &DaqDeviceManager::inputDataReady);
        m_controllers.insert(id, created);
        return created;
    }
}
