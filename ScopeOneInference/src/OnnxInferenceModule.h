#pragma once

#include "scopeone/ProcessingPlugin.h"
#include "scopeone/inference/OnnxRuntime.h"
#include "scopeone/inference/OnnxSession.h"

#include <memory>

namespace scopeone::inference
{
    class OnnxInferenceModule final : public core::ProcessingModule
    {
    public:
        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<core::ProcessingModule> createRuntime() const override;
        core::ProcessingResult process(const core::ImageFrame& frame,
                                       int processingBitDepth) override;

    private:
        void createSession();

        QString m_modelPath;
        int m_provider{0};
        int m_layout{0};
        double m_inputScale{1.0};
        double m_inputMean{0.0};
        double m_inputStd{1.0};
        int m_outputMode{0};
        std::unique_ptr<OnnxRuntime> m_runtime;
        std::unique_ptr<OnnxSession> m_session;
    };
}
