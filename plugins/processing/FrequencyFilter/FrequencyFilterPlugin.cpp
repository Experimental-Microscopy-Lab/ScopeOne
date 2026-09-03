#include "CudaFrequencyFilterModule.h"

#include <QObject>

namespace
{
    class FrequencyFilterPlugin final : public QObject,
                                        public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        QList<scopeone::core::ProcessingModuleDescriptor> processingModules() const override
        {
            scopeone::core::ProcessingModuleDescriptor descriptor;
            descriptor.id = QStringLiteral("cuda.frequency_filter");
            descriptor.name = QStringLiteral("CUDA Frequency Filter");

            scopeone::core::ProcessingParameterDescriptor low;
            low.key = QStringLiteral("low_cutoff");
            low.name = QStringLiteral("Low Cutoff");
            low.type = scopeone::core::ProcessingParameterType::Real;
            low.defaultValue = 0.01;
            low.minimum = 0.0;
            low.maximum = 0.5;
            low.step = 0.01;
            low.decimals = 3;
            descriptor.parameters.append(low);

            scopeone::core::ProcessingParameterDescriptor high;
            high.key = QStringLiteral("high_cutoff");
            high.name = QStringLiteral("High Cutoff");
            high.type = scopeone::core::ProcessingParameterType::Real;
            high.defaultValue = 0.25;
            high.minimum = 0.0;
            high.maximum = 0.5;
            high.step = 0.01;
            high.decimals = 3;
            descriptor.parameters.append(high);

            return {descriptor};
        }

        std::unique_ptr<scopeone::core::ProcessingModule> createProcessingModule(
            const QString& moduleId) override
        {
            return moduleId == QStringLiteral("cuda.frequency_filter")
                       ? std::make_unique<scopeone::cuda_plugin::CudaFrequencyFilterModule>()
                       : nullptr;
        }
    };
}

#include "FrequencyFilterPlugin.moc"
