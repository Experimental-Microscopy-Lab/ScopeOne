#include "internal/cuda/CudaFrequencyFilterModule.h"

#include "internal/cuda/CudaFrequencyFilterKernels.h"
#include "internal/ProcessingModuleRegistry.h"
#include "internal/ProcessingParameters.h"

#include <algorithm>
#include <cuda_runtime.h>

namespace scopeone::core::internal::cuda
{
    CudaFrequencyFilterModule::CudaFrequencyFilterModule()
        : CudaModule(scopeone::core::internal::cuda::GpuMemoryLayout::Contiguous)
    {
    }

    CudaFrequencyFilterModule::~CudaFrequencyFilterModule()
    {
        destroyPlans();
        releaseScratch();
    }

    QString CudaFrequencyFilterModule::id() const
    {
        return QStringLiteral("cuda.frequency_filter");
    }

    QString CudaFrequencyFilterModule::name() const
    {
        return QStringLiteral("Frequency Domain Filter (CUDA)");
    }

    QVariantMap CudaFrequencyFilterModule::parameters() const
    {
        return {{QStringLiteral("min_feature_size"), m_minFeatureSize},
                {QStringLiteral("max_feature_size"), m_maxFeatureSize},
                {QStringLiteral("filter_kind"), m_filterKind},
                {QStringLiteral("output_mode"), m_outputMode}};
    }

    void CudaFrequencyFilterModule::setParameters(const QVariantMap& parameters)
    {
        if (parameters.contains(QStringLiteral("min_feature_size")))
        {
            m_minFeatureSize = qMax(0.0f,
                                     parameters.value(QStringLiteral("min_feature_size")).toFloat());
        }
        if (parameters.contains(QStringLiteral("max_feature_size")))
        {
            m_maxFeatureSize = qMax(0.0f,
                                     parameters.value(QStringLiteral("max_feature_size")).toFloat());
        }
        if (m_minFeatureSize > m_maxFeatureSize)
        {
            std::swap(m_minFeatureSize, m_maxFeatureSize);
        }
        m_filterKind = qBound(0, parameters.value(QStringLiteral("filter_kind"), m_filterKind).toInt(), 1);
        m_outputMode = qBound(0, parameters.value(QStringLiteral("output_mode"), m_outputMode).toInt(), 2);
    }

    std::unique_ptr<scopeone::core::ProcessingModule>
    CudaFrequencyFilterModule::createRuntime() const
    {
        auto runtime = std::make_unique<CudaFrequencyFilterModule>();
        runtime->setParameters(parameters());
        return runtime;
    }

    bool CudaFrequencyFilterModule::resetState()
    {
        destroyPlans();
        releaseScratch();
        return true;
    }

    void CudaFrequencyFilterModule::destroyPlans()
    {
        if (m_forwardPlan != 0)
        {
            cufftDestroy(m_forwardPlan);
            m_forwardPlan = 0;
        }
        if (m_inversePlan != 0)
        {
            cufftDestroy(m_inversePlan);
            m_inversePlan = 0;
        }
        m_planWidth = 0;
        m_planHeight = 0;
    }

    void CudaFrequencyFilterModule::releaseScratch()
    {
        if (m_spectrum)
        {
            cudaFree(m_spectrum);
            m_spectrum = nullptr;
        }
        if (m_minMaxScratch)
        {
            cudaFree(m_minMaxScratch);
            m_minMaxScratch = nullptr;
        }
    }

    bool CudaFrequencyFilterModule::processDevice(
        const scopeone::core::internal::cuda::GpuImage& input,
        scopeone::core::internal::cuda::GpuImage& output,
        int bitDepth)
    {
        if (m_planWidth != input.width() || m_planHeight != input.height())
        {
            destroyPlans();
            if (m_spectrum)
            {
                cudaFree(m_spectrum);
                m_spectrum = nullptr;
            }
            // The R2C spectrum of a width x height image holds (width / 2 + 1) x height values
            const std::size_t spectrumBytes = static_cast<std::size_t>(input.width() / 2 + 1)
                * static_cast<std::size_t>(input.height()) * sizeof(cufftComplex);
            if (cudaMalloc(&m_spectrum, spectrumBytes) != cudaSuccess
                || cufftPlan2d(&m_forwardPlan,
                               input.height(),
                               input.width(),
                               CUFFT_R2C) != CUFFT_SUCCESS)
            {
                return false;
            }
            m_planWidth = input.width();
            m_planHeight = input.height();
        }
        if (m_outputMode == 2 && m_inversePlan == 0
            && cufftPlan2d(&m_inversePlan,
                           input.height(),
                           input.width(),
                           CUFFT_C2R) != CUFFT_SUCCESS)
        {
            return false;
        }
        if (!m_minMaxScratch
            && cudaMalloc(&m_minMaxScratch, 2 * sizeof(unsigned int)) != cudaSuccess)
        {
            return false;
        }
        return scopeone::core::internal::cuda::detail::launchFrequencyFilter(input.data(),
                                                            output.data(),
                                                            m_spectrum,
                                                            input.width(),
                                                            input.height(),
                                                            m_minFeatureSize,
                                                            m_maxFeatureSize,
                                                            m_filterKind,
                                                            m_outputMode,
                                                            m_forwardPlan,
                                                            m_inversePlan,
                                                            m_minMaxScratch,
                                                            bitDepth >= 16 ? 65535.0f : 255.0f);
    }
}

SCOPEONE_REGISTER_PROCESSING_MODULE(
    scopeone::core::internal::cuda::CudaFrequencyFilterModule,
    QStringLiteral("cuda.frequency_filter"),
    QStringLiteral("Frequency Domain Filter (CUDA)"),
    1,
    {scopeone::core::internal::choiceParameter(
         "output_mode", "Output", 2, {"Spectrum", "Filtered spectrum", "Filtered image"}),
     scopeone::core::internal::shownWhen(
         scopeone::core::internal::realParameter(
             "min_feature_size", "Min feature size", 2.0, 0.0, 1000.0, 0.1, 2),
         "output_mode", {1, 2}),
     scopeone::core::internal::shownWhen(
         scopeone::core::internal::realParameter(
             "max_feature_size", "Max feature size", 10.0, 0.0, 1000.0, 0.1, 2),
         "output_mode", {1, 2}),
     scopeone::core::internal::shownWhen(
         scopeone::core::internal::choiceParameter("filter_kind", "Filter kind", 0, {"Smooth", "Hard"}),
         "output_mode", {1, 2})})
