#pragma once

#include "internal/ProcessingModule.h"

#include <deque>
#include <vector>

namespace scopeone::core::internal
{
    class SpatiotemporalBinningModule : public ProcessingModule
    {
    public:
        enum class BinningMode
        {
            Mean = 0,
            Sum = 1,
            Minimum = 2,
            Maximum = 3,
            Skip = 4
        };

        ProcessingModuleKind kind() const noexcept override { return ProcessingModuleKind::SpatiotemporalBinning; }
        QString name() const override { return "Spatiotemporal Binning"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        bool resetState() override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        int m_spatialBinX{1};
        int m_spatialBinY{1};
        int m_temporalBin{1};
        BinningMode m_spatialMode{BinningMode::Mean};
        BinningMode m_temporalMode{BinningMode::Mean};
        std::deque<ImageFrame> m_frameBuffer;
        std::vector<qint64> m_temporalSum;
    };
}
