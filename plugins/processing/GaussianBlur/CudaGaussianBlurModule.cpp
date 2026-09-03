#include "CudaGaussianBlurModule.h"

#include "scopeone/gpu/CudaKernelLaunch.h"

namespace scopeone::cuda_plugin
{
    CudaGaussianBlurModule::CudaGaussianBlurModule()
        : CudaRealImageModule(scopeone::gpu::GpuMemoryLayout::Pitched2D)
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
        const scopeone::gpu::GpuRealFrame& input,
        scopeone::gpu::GpuRealFrame& output,
        int)
    {
        return scopeone::gpu::detail::launchGaussian(input.data(),
                                                     input.pitchBytes(),
                                                     output.data(),
                                                     output.pitchBytes(),
                                                     input.width(),
                                                     input.height(),
                                                     m_sigma);
    }
}
