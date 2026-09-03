#pragma once

#include "scopeone/cuda/CudaRealImageModule.h"

namespace scopeone::cuda_plugin
{
    class CudaGaussianBlurModule final : public scopeone::cuda::CudaRealImageModule
    {
    public:
        CudaGaussianBlurModule();

        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override;

    protected:
        bool processDevice(const scopeone::cuda::GpuRealFrame& input,
                           scopeone::cuda::GpuRealFrame& output,
                           int bitDepth) override;

    private:
        float m_sigma{1.5f};
    };
}
