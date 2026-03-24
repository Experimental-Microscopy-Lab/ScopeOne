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

    explicit FFTModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "FFT Bandpass"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

private:
    double m_minWidth{2.0};
    double m_maxWidth{10.0};
    FilterKind m_filterKind{FilterKind::Smooth};
};

}
