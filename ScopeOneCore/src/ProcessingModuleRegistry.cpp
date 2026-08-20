#include "internal/ProcessingModuleRegistry.h"

#include "internal/BackgroundCalibrationModule.h"
#include "internal/DifferentialRollingModule.h"
#include "internal/FFTModule.h"
#include "internal/GaussianBlurModule.h"
#include "internal/SpatiotemporalBinningModule.h"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QSet>

namespace scopeone::core::internal
{
    namespace
    {
        bool validDescriptor(const ProcessingModuleDescriptor& descriptor)
        {
            QSet<QString> parameterKeys;
            for (const ProcessingParameterDescriptor& parameter : descriptor.parameters)
            {
                const QString key = parameter.key.trimmed();
                if (key.isEmpty()
                    || parameter.name.trimmed().isEmpty()
                    || parameterKeys.contains(key))
                {
                    return false;
                }
                parameterKeys.insert(key);
            }
            return !descriptor.id.trimmed().isEmpty()
                   && !descriptor.name.trimmed().isEmpty();
        }

        ProcessingParameterDescriptor integerParameter(const char* key,
                                                       const char* name,
                                                       int value,
                                                       int minimum,
                                                       int maximum,
                                                       int step = 1)
        {
            return {QString::fromLatin1(key),
                    QString::fromLatin1(name),
                    ProcessingParameterType::Integer,
                    value,
                    minimum,
                    maximum,
                    step};
        }

        ProcessingParameterDescriptor realParameter(const char* key,
                                                    const char* name,
                                                    double value,
                                                    double minimum,
                                                    double maximum,
                                                    double step,
                                                    int decimals)
        {
            ProcessingParameterDescriptor descriptor{
                QString::fromLatin1(key),
                QString::fromLatin1(name),
                ProcessingParameterType::Real,
                value,
                minimum,
                maximum,
                step};
            descriptor.decimals = decimals;
            return descriptor;
        }

        ProcessingParameterDescriptor booleanParameter(const char* key,
                                                       const char* name,
                                                       bool value)
        {
            return {QString::fromLatin1(key),
                    QString::fromLatin1(name),
                    ProcessingParameterType::Boolean,
                    value};
        }

        ProcessingParameterDescriptor choiceParameter(
            const char* key,
            const char* name,
            int value,
            std::initializer_list<const char*> choices)
        {
            ProcessingParameterDescriptor descriptor{
                QString::fromLatin1(key),
                QString::fromLatin1(name),
                ProcessingParameterType::Choice,
                value};
            int index = 0;
            for (const char* choice : choices)
            {
                descriptor.choices.append({QString::fromLatin1(choice), index++});
            }
            return descriptor;
        }

        template<typename Module>
        bool registerBuiltIn(ProcessingModuleRegistry& registry,
                             ProcessingModuleDescriptor descriptor)
        {
            return registry.registerModule(descriptor, []()
            {
                return std::make_unique<Module>();
            });
        }

        void registerBuiltIns(ProcessingModuleRegistry& registry)
        {
            registerBuiltIn<SpatiotemporalBinningModule>(
                registry,
                {QStringLiteral("spatiotemporal_binning"),
                 QStringLiteral("Spatiotemporal Binning"),
                 1,
                 {integerParameter("spatial_bin_x", "Spatial X", 1, 1, 64),
                  integerParameter("spatial_bin_y", "Spatial Y", 1, 1, 64),
                  integerParameter("temporal_bin", "Temporal", 1, 1, 256),
                  choiceParameter("spatial_mode", "Spatial mode", 0,
                                  {"Mean", "Sum", "Minimum", "Maximum", "Skip"}),
                  choiceParameter("temporal_mode", "Temporal mode", 0,
                                  {"Mean", "Sum", "Minimum", "Maximum", "Skip"})},
                 true});

            registerBuiltIn<GaussianBlurModule>(
                registry,
                {QStringLiteral("gaussian_blur"),
                 QStringLiteral("Gaussian Blur"),
                 1,
                 {integerParameter("kernel_size", "Kernel size", 3, 1, 99, 2),
                  realParameter("sigma", "Sigma", 0.0, 0.0, 100.0, 0.1, 2)}});

            registerBuiltIn<FFTModule>(
                registry,
                {QStringLiteral("fft"),
                 QStringLiteral("FFT"),
                 1,
                 {choiceParameter("output_mode", "Output", 2,
                                  {"FFT Spectrum", "Bandpass FFT Spectrum", "Bandpass IFFT Image"}),
                  realParameter("min_feature_size", "Min feature size", 2.0, 0.0, 1000.0, 0.1, 2),
                  realParameter("max_feature_size", "Max feature size", 10.0, 0.0, 1000.0, 0.1, 2),
                  choiceParameter("filter_kind", "Filter kind", 0, {"Smooth", "Hard"})}});

            registerBuiltIn<DifferentialRollingModule>(
                registry,
                {QStringLiteral("differential_rolling"),
                 QStringLiteral("Differential Rolling"),
                 1,
                 {integerParameter("batch_size", "Batch size", 16, 1, 256),
                  booleanParameter("normalize", "Normalize by batch 1", true)},
                 true});

            registerBuiltIn<BackgroundCalibrationModule>(
                registry,
                {QStringLiteral("background_calibration"),
                 QStringLiteral("Background Calibration"),
                 1,
                 {integerParameter("calibration_frames", "Frames", 101, 3, 1001, 2),
                  choiceParameter("mode", "Mode", 0, {"Snapshot", "Running"}),
                  choiceParameter("method", "Method", 0,
                                  {"Median", "Mean", "Maximum", "Minimum"}),
                  choiceParameter("operation", "Operation", 0,
                                  {"Subtract", "Add", "Multiply", "Divide"})},
                 true});
        }
    }

    ProcessingModuleRegistry::ProcessingModuleRegistry()
    {
        registerBuiltIns(*this);
    }

    ProcessingModuleRegistry::~ProcessingModuleRegistry() = default;

    bool ProcessingModuleRegistry::registerModule(const ProcessingModuleDescriptor& descriptor,
                                                  Factory factory)
    {
        const QString id = descriptor.id.trimmed();
        if (!validDescriptor(descriptor) || !factory || m_entries.contains(id))
        {
            return false;
        }

        ProcessingModuleDescriptor normalized = descriptor;
        normalized.id = id;
        m_entries.insert(id, {std::move(normalized), std::move(factory)});
        m_order.append(id);
        return true;
    }

    QList<ProcessingModuleDescriptor> ProcessingModuleRegistry::descriptors() const
    {
        QList<ProcessingModuleDescriptor> result;
        result.reserve(m_order.size());
        for (const QString& id : m_order)
        {
            result.append(m_entries.value(id).descriptor);
        }
        return result;
    }

    ProcessingModuleDescriptor ProcessingModuleRegistry::descriptor(const QString& moduleId) const
    {
        return m_entries.value(moduleId.trimmed()).descriptor;
    }

    std::unique_ptr<ProcessingModule> ProcessingModuleRegistry::create(const QString& moduleId) const
    {
        const auto it = m_entries.constFind(moduleId.trimmed());
        if (it == m_entries.constEnd())
        {
            return {};
        }
        std::unique_ptr<ProcessingModule> module = it->factory();
        return module && module->id() == it->descriptor.id ? std::move(module) : nullptr;
    }

    QStringList ProcessingModuleRegistry::loadPlugins(const QString& directoryPath)
    {
        QStringList errors;
        const QDir directory(directoryPath);
        for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
        {
            if (!QLibrary::isLibrary(file.absoluteFilePath()))
            {
                continue;
            }

            auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
            QObject* instance = loader->instance();
            auto* plugin = qobject_cast<ProcessingPlugin*>(instance);
            if (!plugin)
            {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(file.fileName(), loader->errorString()));
                continue;
            }

            const QList<ProcessingModuleDescriptor> pluginDescriptors = plugin->processingModules();
            QSet<QString> pluginIds;
            bool valid = !pluginDescriptors.isEmpty();
            for (const ProcessingModuleDescriptor& descriptor : pluginDescriptors)
            {
                const QString id = descriptor.id.trimmed();
                if (!validDescriptor(descriptor)
                    || pluginIds.contains(id)
                    || m_entries.contains(id))
                {
                    valid = false;
                    break;
                }
                pluginIds.insert(id);
            }
            if (!valid)
            {
                errors.append(QStringLiteral("%1: invalid or duplicate processing module id")
                                  .arg(file.fileName()));
                loader->unload();
                continue;
            }

            for (const ProcessingModuleDescriptor& descriptor : pluginDescriptors)
            {
                const QString id = descriptor.id.trimmed();
                registerModule(descriptor, [plugin, id]()
                {
                    return plugin->createProcessingModule(id);
                });
            }
            m_pluginLoaders.push_back(std::move(loader));
        }
        return errors;
    }
}
