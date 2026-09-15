#pragma once

#include "scopeone/ProcessingPlugin.h"

#include <deque>
#include <opencv2/core.hpp>

namespace scopeone::iscat
{
    class IscatProcessingModule final : public scopeone::core::ProcessingModule
    {
    public:
        QString id() const override;
        QString name() const override;
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override;
        bool resetState() override;
        scopeone::core::ProcessingResult process(const scopeone::core::ImageFrame& frame,
                                                 int processingBitDepth) override;

    private:
        void clearState();
        cv::Mat computePixelMedian(const std::deque<cv::Mat>& frames) const;
        scopeone::core::ProcessingResult processFlatField(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processTemporalMedian(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processEma(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processSnapshot(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processDra(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult finalizeContrastOutput(
            const cv::Mat& input,
            const cv::Mat& background,
            const scopeone::core::ImageFrame& frame);

        int m_mode{0};
        double m_blurSigma{15.0};
        int m_medianWindow{31};
        double m_emaAlpha{0.05};
        bool m_captureReference{false};
        int m_batchSize{16};
        double m_contrastGain{20.0};
        bool m_enableHighPass{false};
        double m_highPassSigma{30.0};
        int m_outputMode{0};
        std::deque<cv::Mat> m_medianBuffer;
        cv::Mat m_lockedMedianBackground;
        cv::Mat m_emaAccumulator;
        cv::Mat m_snapshotBackground;
        std::deque<cv::Mat> m_draBatchA;
        std::deque<cv::Mat> m_draBatchB;
        cv::Mat m_draSumA;
        cv::Mat m_draSumB;
        cv::Size m_stateSize;
    };
}
