#pragma once

#include "internal/ProcessingModule.h"

#include <deque>
#include <vector>

namespace scopeone::core::internal
{
    class DifferentialRollingModule : public ProcessingModule
    {
    public:
        ProcessingModuleKind kind() const noexcept override { return ProcessingModuleKind::DifferentialRolling; }
        QString name() const override { return "Differential Rolling"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        bool resetState() override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

        struct CameraState
        {
            int width{0};
            int height{0};
            std::deque<ImageFrame> batchA;
            std::deque<ImageFrame> batchB;
            std::vector<int> sumA;
            std::vector<int> sumB;
        };

    private:
        int m_batchSize{16};
        bool m_normalize{true};
        CameraState m_state;
    };
} // namespace scopeone::core::internal
