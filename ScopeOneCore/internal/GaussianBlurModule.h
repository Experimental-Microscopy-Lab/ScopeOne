#pragma once

#include "internal/ProcessingModule.h"

namespace scopeone::core::internal
{
    class GaussianBlurModule : public ProcessingModule
    {
    public:
        QString id() const override { return QStringLiteral("gaussian_blur"); }
        QString name() const override { return "Gaussian Blur"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        int m_kernelSize{3};
        double m_sigma{0.0};
    };
}
