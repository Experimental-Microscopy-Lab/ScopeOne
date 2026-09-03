#pragma once

#include "scopeone/cuda/CudaRealImageModule.h"
#include "scopeone/cuda/GpuComplexFrame.h"

#include <cufft.h>

namespace scopeone::cuda_plugin
{
    class CudaFrequencyFilterModule final : public scopeone::cuda::CudaRealImageModule
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
        bool processDevice(const scopeone::cuda::GpuRealFrame& input,
                           scopeone::cuda::GpuRealFrame& output,
                           int bitDepth) override;

    private:
        void destroyPlans();

        scopeone::cuda::GpuComplexFrame m_spectrum;
        cufftHandle m_forwardPlan{0};
        cufftHandle m_inversePlan{0};
        int m_planWidth{0};
        int m_planHeight{0};
        float m_lowCutoff{0.01f};
        float m_highCutoff{0.25f};
    };
}
