#pragma once

#include "internal/ProcessingModule.h"

#include <deque>
#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    // iSCAT contrast (frame - background) / background, with the background from a blur of the
    // frame (Flat Field), the mean of frames taken on Capture Reference (Reference), or the batch
    // of frames before the latest batch (DRA)
    class IscatModule final : public scopeone::core::ProcessingModule
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
        scopeone::core::ProcessingResult processFlatField(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processReference(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult processDra(
            const cv::Mat& input,
            const scopeone::core::ImageFrame& frame);
        scopeone::core::ProcessingResult contrastOutput(
            const cv::Mat& input,
            const cv::Mat& background,
            const scopeone::core::ImageFrame& frame);

        int m_mode{0};
        int m_frames{16};
        double m_blurSigma{15.0};
        bool m_captureReference{false};
        double m_contrastGain{20.0};
        cv::Mat m_referenceSum;
        int m_referenceCount{0};
        cv::Mat m_reference;
        std::deque<cv::Mat> m_draBatchA;
        std::deque<cv::Mat> m_draBatchB;
        cv::Mat m_draSumA;
        cv::Mat m_draSumB;
        cv::Size m_stateSize;
    };
}
