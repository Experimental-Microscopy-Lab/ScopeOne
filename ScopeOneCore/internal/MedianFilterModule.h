#pragma once

#include "internal/ProcessingModule.h"
#include <deque>

namespace scopeone::core::internal {

class MedianFilterModule : public ProcessingModule
{
    Q_OBJECT

public:
    explicit MedianFilterModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "Temporal Median"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

    void resetBuffer();

private:
    int m_windowSize;
    std::deque<ImageFrame> m_frameBuffer;
};

}
