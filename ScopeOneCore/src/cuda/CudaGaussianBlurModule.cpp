#include "internal/cuda/CudaGaussianBlurModule.h"

#include "internal/cuda/CudaGaussianBlurKernels.h"
#include "internal/ProcessingModuleRegistry.h"
#include "internal/ProcessingParameters.h"

namespace scopeone::core::internal::cuda
{
    CudaGaussianBlurModule::CudaGaussianBlurModule()
        : CudaModule(scopeone::core::internal::cuda::GpuMemoryLayout::Pitched2D)
    {
    }

    QString CudaGaussianBlurModule::id() const
    {
        return QStringLiteral("cuda.gaussian_blur");
    }

    QString CudaGaussianBlurModule::name() const
    {
        return QStringLiteral("Gaussian Blur (CUDA)");
    }

    QVariantMap CudaGaussianBlurModule::parameters() const
    {
        return {{QStringLiteral("kernel_size"), m_kernelSize},
                {QStringLiteral("sigma"), m_sigma}};
    }

    void CudaGaussianBlurModule::setParameters(const QVariantMap& parameters)
    {
        if (parameters.contains(QStringLiteral("kernel_size")))
        {
            m_kernelSize = qMax(1, parameters.value(QStringLiteral("kernel_size")).toInt());
            if ((m_kernelSize % 2) == 0)
            {
                ++m_kernelSize;
            }
        }
        if (parameters.contains(QStringLiteral("sigma")))
        {
            m_sigma = qMax(0.0f, parameters.value(QStringLiteral("sigma")).toFloat());
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
        const scopeone::core::internal::cuda::GpuImage& input,
        scopeone::core::internal::cuda::GpuImage& output,
        int)
    {
        return scopeone::core::internal::cuda::detail::launchGaussian(input.data(),
                                                     input.pitchBytes(),
                                                     output.data(),
                                                     output.pitchBytes(),
                                                     input.width(),
                                                     input.height(),
                                                     m_kernelSize,
                                                     m_sigma);
    }
}

SCOPEONE_REGISTER_PROCESSING_MODULE(
    scopeone::core::internal::cuda::CudaGaussianBlurModule,
    QStringLiteral("cuda.gaussian_blur"),
    QStringLiteral("Gaussian Blur (CUDA)"),
    1,
    {scopeone::core::internal::integerParameter("kernel_size", "Kernel size", 3, 1, 99, 2),
     scopeone::core::internal::realParameter("sigma", "Sigma", 0.0, 0.0, 100.0, 0.1, 2)})
