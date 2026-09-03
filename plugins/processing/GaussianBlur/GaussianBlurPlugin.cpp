#include "CudaGaussianBlurModule.h"

#include <QObject>

namespace
{
    class GaussianBlurPlugin final : public QObject,
                                     public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        QList<scopeone::core::ProcessingModuleDescriptor> processingModules() const override
        {
            scopeone::core::ProcessingModuleDescriptor descriptor;
            descriptor.id = QStringLiteral("cuda.gaussian_blur");
            descriptor.name = QStringLiteral("CUDA Gaussian Blur");

            scopeone::core::ProcessingParameterDescriptor sigma;
            sigma.key = QStringLiteral("sigma");
            sigma.name = QStringLiteral("Sigma");
            sigma.type = scopeone::core::ProcessingParameterType::Real;
            sigma.defaultValue = 1.5;
            sigma.minimum = 0.1;
            sigma.maximum = 20.0;
            sigma.step = 0.1;
            sigma.decimals = 2;
            descriptor.parameters.append(sigma);

            return {descriptor};
        }

        std::unique_ptr<scopeone::core::ProcessingModule> createProcessingModule(
            const QString& moduleId) override
        {
            return moduleId == QStringLiteral("cuda.gaussian_blur")
                       ? std::make_unique<scopeone::cuda_plugin::CudaGaussianBlurModule>()
                       : nullptr;
        }
    };
}

#include "GaussianBlurPlugin.moc"
