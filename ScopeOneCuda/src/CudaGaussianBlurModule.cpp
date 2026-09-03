#include "CudaGaussianBlurModule.h"

#include "scopeone/cuda/CudaKernelLaunch.h"

namespace scopeone::cuda_plugin
{
    CudaGaussianBlurModule::CudaGaussianBlurModule()
        : CudaRealImageModule(scopeone::cuda::GpuMemoryLayout::Pitched2D)
    {
    }

    QString CudaGaussianBlurModule::id() const
    {
        return QStringLiteral("cuda.gaussian_blur");
    }

    QString CudaGaussianBlurModule::name() const
    {
        return QStringLiteral("CUDA Gaussian Blur");
    }

    QVariantMap CudaGaussianBlurModule::parameters() const
    {
        return {{QStringLiteral("sigma"), m_sigma}};
    }

    void CudaGaussianBlurModule::setParameters(const QVariantMap& parameters)
    {
        if (parameters.contains(QStringLiteral("sigma")))
        {
            m_sigma = parameters.value(QStringLiteral("sigma")).toFloat();
        }
    }

    std::unique_ptr<scopeone::core::ProcessingModule>
    CudaGaussianBlurModule::createRuntime() const
    {
        auto runtime = std::make_unique<CudaGaussianBlurModule>();
        runtime->setParameters(parameters());
        return runtime;
    }

    bool CudaGaussianBlurModule::processDevice(
        const scopeone::cuda::GpuRealFrame& input,
        scopeone::cuda::GpuRealFrame& output,
        int)
    {
        return scopeone::cuda::detail::launchGaussian(input.data(),
                                                     input.pitchBytes(),
                                                     output.data(),
                                                     output.pitchBytes(),
                                                     input.width(),
                                                     input.height(),
                                                     m_sigma);
    }
}
