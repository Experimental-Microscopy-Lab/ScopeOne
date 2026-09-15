#pragma once

#include "internal/ProcessingModule.h"

#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    class FFTModule : public ProcessingModule
    {
    public:
        QString id() const override { return QStringLiteral("fft"); }
        QString name() const override { return "FFT"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;
        ProcessingResult processValue(const ProcessingValue& input,
                                       int processingBitDepth) override;

    private:
        cv::Mat m_grayFloat;
        cv::Mat m_padded;
        cv::Mat m_complex;
        cv::Mat m_planes[2];
        cv::Mat m_spectrumMagnitude;
        cv::Mat m_shiftedSpectrum;
    };
}
