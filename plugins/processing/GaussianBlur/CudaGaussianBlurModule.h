#pragma once

#include "scopeone/gpu/CudaRealImageModule.h"

namespace scopeone::cuda_plugin
{
    class CudaGaussianBlurModule final : public scopeone::gpu::CudaRealImageModule
    {
    public:
        CudaGaussianBlurModule();

        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override;

    protected:
        bool processDevice(const scopeone::gpu::GpuRealFrame& input,
                           scopeone::gpu::GpuRealFrame& output,
                           int bitDepth) override;

    private:
        float m_sigma{1.5f};
    };
}
