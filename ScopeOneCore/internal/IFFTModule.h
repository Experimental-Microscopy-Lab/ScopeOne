#pragma once

#include "internal/ProcessingModule.h"

namespace scopeone::core::internal
{
    class IFFTModule final : public ProcessingModule
    {
    public:
        QString id() const override { return QStringLiteral("ifft"); }
        QString name() const override { return QStringLiteral("IFFT"); }
        QVariantMap parameters() const override { return {}; }
        void setParameters(const QVariantMap&) override {}
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;
        ProcessingResult processValue(const ProcessingValue& input,
                                       int processingBitDepth) override;
    };
}
