#include "CudaFrequencyFilterModule.h"

#include "scopeone/cuda/CudaKernelLaunch.h"

#include <algorithm>
#include <cuda_runtime.h>

namespace scopeone::cuda_plugin
{
    CudaFrequencyFilterModule::CudaFrequencyFilterModule()
        : CudaRealImageModule(scopeone::cuda::GpuMemoryLayout::Contiguous)
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
        return QStringLiteral("CUDA Frequency Filter");
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
        m_spectrum.release();
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
        if (m_minMaxScratch)
        {
            cudaFree(m_minMaxScratch);
            m_minMaxScratch = nullptr;
        }
    }

    bool CudaFrequencyFilterModule::processDevice(
        const scopeone::cuda::GpuRealFrame& input,
        scopeone::cuda::GpuRealFrame& output,
        int bitDepth)
    {
        if (m_planWidth != input.width() || m_planHeight != input.height())
        {
            destroyPlans();
            if (!m_spectrum.allocateForRealImage(input.width(), input.height())
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
        return scopeone::cuda::detail::launchFrequencyFilter(input.data(),
                                                            output.data(),
                                                            m_spectrum.data(),
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
