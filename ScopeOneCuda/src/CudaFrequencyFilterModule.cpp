#include "CudaFrequencyFilterModule.h"

#include "scopeone/cuda/CudaKernelLaunch.h"

namespace scopeone::cuda_plugin
{
    CudaFrequencyFilterModule::CudaFrequencyFilterModule()
        : CudaRealImageModule(scopeone::cuda::GpuMemoryLayout::Contiguous)
    {
    }

    CudaFrequencyFilterModule::~CudaFrequencyFilterModule()
    {
        destroyPlans();
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
        return {{QStringLiteral("low_cutoff"), m_lowCutoff},
                {QStringLiteral("high_cutoff"), m_highCutoff}};
    }

    void CudaFrequencyFilterModule::setParameters(const QVariantMap& parameters)
    {
        if (parameters.contains(QStringLiteral("low_cutoff")))
        {
            m_lowCutoff = parameters.value(QStringLiteral("low_cutoff")).toFloat();
        }
        if (parameters.contains(QStringLiteral("high_cutoff")))
        {
            m_highCutoff = parameters.value(QStringLiteral("high_cutoff")).toFloat();
        }
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

    bool CudaFrequencyFilterModule::processDevice(
        const scopeone::cuda::GpuRealFrame& input,
        scopeone::cuda::GpuRealFrame& output,
        int)
    {
        if (m_planWidth != input.width() || m_planHeight != input.height())
        {
            destroyPlans();
            if (!m_spectrum.allocateForRealImage(input.width(), input.height())
                || cufftPlan2d(&m_forwardPlan,
                               input.height(),
                               input.width(),
                               CUFFT_R2C) != CUFFT_SUCCESS
                || cufftPlan2d(&m_inversePlan,
                               input.height(),
                               input.width(),
                               CUFFT_C2R) != CUFFT_SUCCESS)
            {
                return false;
            }
            m_planWidth = input.width();
            m_planHeight = input.height();
        }
        return scopeone::cuda::detail::launchFrequencyFilter(input.data(),
                                                            output.data(),
                                                            m_spectrum.data(),
                                                            input.width(),
                                                            input.height(),
                                                            m_lowCutoff,
                                                            m_highCutoff,
                                                            m_forwardPlan,
                                                            m_inversePlan);
    }
}
