#pragma once

#include "internal/ProcessingModule.h"

namespace scopeone::core::internal {

class TemplateModule : public ProcessingModule
{
    Q_OBJECT

public:
    explicit TemplateModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "Template Module"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

private:
    int m_exampleParameter{10};
};

}
