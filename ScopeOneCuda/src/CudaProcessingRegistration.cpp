#include "CudaFrequencyFilterModule.h"
#include "CudaGaussianBlurModule.h"

#include "scopeone/ScopeOneCore.h"

extern "C" SCOPEONE_CUDA_EXPORT void scopeone_register_processing_modules(
    scopeone::core::ScopeOneCore* core)
{
    core->registerProcessingModule(
        {QStringLiteral("cuda.gaussian_blur"),
         QStringLiteral("CUDA Gaussian Blur"),
         1,
         {{QStringLiteral("sigma"),
           QStringLiteral("Sigma"),
           scopeone::core::ProcessingParameterType::Real,
           1.5,
           0.1,
           20.0,
           0.1,
           2}}},
        []()
        {
            return std::make_unique<scopeone::cuda_plugin::CudaGaussianBlurModule>();
        });

    core->registerProcessingModule(
        {QStringLiteral("cuda.frequency_filter"),
         QStringLiteral("CUDA Frequency Filter"),
         1,
         {{QStringLiteral("low_cutoff"),
           QStringLiteral("Low Cutoff"),
           scopeone::core::ProcessingParameterType::Real,
           0.01,
           0.0,
           0.5,
           0.01,
           3},
          {QStringLiteral("high_cutoff"),
           QStringLiteral("High Cutoff"),
           scopeone::core::ProcessingParameterType::Real,
           0.25,
           0.0,
           0.5,
           0.01,
           3}}},
        []()
        {
            return std::make_unique<scopeone::cuda_plugin::CudaFrequencyFilterModule>();
        });
}
