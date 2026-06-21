#pragma once

#include "internal/ProcessingModule.h"
namespace scopeone::core::internal {

class FFTModule : public ProcessingModule
{
    Q_OBJECT

public:
    enum class FilterKind {
        Smooth = 0,
        Hard = 1
    };

    enum class OutputMode {
        Spectrum = 0,
        BandpassSpectrum = 1,
        BandpassImage = 2
    };

    explicit FFTModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "FFT"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

private:
    double m_minFeatureSize{2.0};
    double m_maxFeatureSize{10.0};
    FilterKind m_filterKind{FilterKind::Smooth};
    OutputMode m_outputMode{OutputMode::BandpassImage};
};

}
