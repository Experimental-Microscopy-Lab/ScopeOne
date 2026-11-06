#pragma once

#include "internal/cuda/CudaModule.h"

namespace scopeone::core::internal::cuda
{
    class CudaGaussianBlurModule final : public scopeone::core::internal::cuda::CudaModule
    {
    public:
        CudaGaussianBlurModule();

        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override;

    protected:
        bool processDevice(const scopeone::core::internal::cuda::GpuImage& input,
                           scopeone::core::internal::cuda::GpuImage& output,
                           int bitDepth) override;

    private:
        int m_kernelSize{3};
        float m_sigma{0.0f};
    };
}
