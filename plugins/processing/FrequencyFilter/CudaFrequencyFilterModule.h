#pragma once

#include "scopeone/gpu/CudaRealImageModule.h"
#include "scopeone/gpu/GpuComplexFrame.h"

#include <cufft.h>

namespace scopeone::cuda_plugin
{
    class CudaFrequencyFilterModule final : public scopeone::gpu::CudaRealImageModule
    {
    public:
        CudaFrequencyFilterModule();
        ~CudaFrequencyFilterModule() override;

        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override;
        bool resetState() override;

    protected:
        bool processDevice(const scopeone::gpu::GpuRealFrame& input,
                           scopeone::gpu::GpuRealFrame& output,
                           int bitDepth) override;

    private:
        void destroyPlans();

        scopeone::gpu::GpuComplexFrame m_spectrum;
        cufftHandle m_forwardPlan{0};
        cufftHandle m_inversePlan{0};
        int m_planWidth{0};
        int m_planHeight{0};
        float m_lowCutoff{0.01f};
        float m_highCutoff{0.25f};
    };
}
