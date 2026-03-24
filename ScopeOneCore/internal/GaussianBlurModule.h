#pragma once

#include "internal/ProcessingModule.h"

namespace scopeone::core::internal {

class GaussianBlurModule : public ProcessingModule
{
    Q_OBJECT

public:
    explicit GaussianBlurModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "Gaussian Blur"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

private:
    int m_kernelSize{3};
    double m_sigma{0.0};
};

}
