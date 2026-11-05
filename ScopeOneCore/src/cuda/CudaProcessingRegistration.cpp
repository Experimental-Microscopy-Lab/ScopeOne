#include "internal/cuda/CudaProcessingRegistration.h"
#include "internal/cuda/CudaFrequencyFilterModule.h"
#include "internal/cuda/CudaGaussianBlurModule.h"

#include "internal/ProcessingModuleRegistry.h"

namespace scopeone::cuda
{
    namespace
    {
        QList<core::ProcessingModuleDescriptor> descriptors()
        {
            return {{QStringLiteral("cuda.gaussian_blur"),
         QStringLiteral("CUDA Gaussian Blur"),
         1,
         {{QStringLiteral("kernel_size"),
           QStringLiteral("Kernel size"),
           scopeone::core::ProcessingParameterType::Integer,
           3,
           1,
           99,
           2,
           0},
          {QStringLiteral("sigma"),
           QStringLiteral("Sigma"),
           scopeone::core::ProcessingParameterType::Real,
           0.0,
           0.0,
           100.0,
           0.1,
           2}}},
                    {QStringLiteral("cuda.frequency_filter"),
                     QStringLiteral("CUDA Frequency Filter"),
                     1,
                     {{QStringLiteral("output_mode"),
                       QStringLiteral("Output"),
                       core::ProcessingParameterType::Choice,
                       2,
                       0,
                       2,
                       1,
                       0,
                       {{QStringLiteral("Spectrum"), 0},
                        {QStringLiteral("Filtered spectrum"), 1},
                        {QStringLiteral("Filtered image"), 2}}},
                      {QStringLiteral("min_feature_size"),
                       QStringLiteral("Min feature size"),
                       core::ProcessingParameterType::Real,
                       2.0,
                       0.0,
                       1000.0,
                       0.1,
                       2},
                      {QStringLiteral("max_feature_size"),
                       QStringLiteral("Max feature size"),
                       core::ProcessingParameterType::Real,
                       10.0,
                       0.0,
                       1000.0,
                       0.1,
                       2},
                      {QStringLiteral("filter_kind"),
                       QStringLiteral("Filter kind"),
                       core::ProcessingParameterType::Choice,
                       0,
                       0,
                       1,
                       1,
                       0,
                       {{QStringLiteral("Smooth"), 0},
                        {QStringLiteral("Hard"), 1}}}}}};
        }

        std::unique_ptr<core::ProcessingModule> create(const QString& moduleId)
        {
            if (moduleId == QStringLiteral("cuda.gaussian_blur"))
            {
                return std::make_unique<CudaGaussianBlurModule>();
            }
            if (moduleId == QStringLiteral("cuda.frequency_filter"))
            {
                return std::make_unique<CudaFrequencyFilterModule>();
            }
            return {};
        }
    }

    void registerCudaProcessingModules(core::internal::ProcessingModuleRegistry& registry)
    {
        for (const core::ProcessingModuleDescriptor& descriptor : descriptors())
        {
            const QString id = descriptor.id;
            registry.registerModule(descriptor, [id]() { return create(id); });
        }
    }
}
