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
        []()
        {
            return std::make_unique<scopeone::cuda_plugin::CudaGaussianBlurModule>();
        });

    core->registerProcessingModule(
        {QStringLiteral("cuda.frequency_filter"),
         QStringLiteral("CUDA Frequency Filter"),
         1,
         {{QStringLiteral("output_mode"),
           QStringLiteral("Output"),
           scopeone::core::ProcessingParameterType::Choice,
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
           scopeone::core::ProcessingParameterType::Real,
           2.0,
           0.0,
           1000.0,
           0.1,
           2},
          {QStringLiteral("max_feature_size"),
           QStringLiteral("Max feature size"),
           scopeone::core::ProcessingParameterType::Real,
           10.0,
           0.0,
           1000.0,
           0.1,
           2},
          {QStringLiteral("filter_kind"),
           QStringLiteral("Filter kind"),
           scopeone::core::ProcessingParameterType::Choice,
           0,
           0,
           1,
           1,
           0,
           {{QStringLiteral("Smooth"), 0}, {QStringLiteral("Hard"), 1}}}}},
        []()
        {
            return std::make_unique<scopeone::cuda_plugin::CudaFrequencyFilterModule>();
        });
}
