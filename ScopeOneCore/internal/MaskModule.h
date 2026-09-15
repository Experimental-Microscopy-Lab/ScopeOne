#pragma once

#include "internal/ProcessingModule.h"

#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    class MaskModule final : public ProcessingModule
    {
    public:
        QString id() const override { return QStringLiteral("mask"); }
        QString name() const override { return QStringLiteral("Mask"); }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;
        ProcessingResult processValue(const ProcessingValue& input,
                                       int processingBitDepth) override;

    private:
        const cv::Mat& maskForSize(const cv::Size& size);
        const cv::Mat& frequencyMaskForSize(const cv::Size& size);
        void invalidateMask();

        int m_shape{0};
        double m_centerX{0.0};
        double m_centerY{0.0};
        double m_sizeX{0.1};
        double m_sizeY{0.1};
        double m_innerSize{0.0};
        double m_rotation{0.0};
        double m_edgeWidth{0.0};
        bool m_invert{false};

        cv::Mat m_mask;
        cv::Mat m_frequencyMask;
        cv::Size m_maskSize;
        QVariantMap m_maskParameters;
    };
}
