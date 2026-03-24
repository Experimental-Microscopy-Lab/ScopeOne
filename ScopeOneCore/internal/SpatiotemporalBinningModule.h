#pragma once

#include "internal/ProcessingModule.h"

#include <deque>

namespace scopeone::core::internal {

class SpatiotemporalBinningModule : public ProcessingModule
{
    Q_OBJECT

public:
    enum class BinningMode {
        Mean = 0,
        Sum = 1,
        Minimum = 2,
        Maximum = 3,
        Skip = 4
    };

    explicit SpatiotemporalBinningModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "Spatiotemporal Binning"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

private:
    int m_spatialBinX{1};
    int m_spatialBinY{1};
    int m_temporalBin{1};
    BinningMode m_spatialMode{BinningMode::Mean};
    BinningMode m_temporalMode{BinningMode::Mean};
    std::deque<ImageFrame> m_frameBuffer;
};

}
