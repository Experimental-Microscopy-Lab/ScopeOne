#pragma once

#include "internal/ProcessingModule.h"

#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    class FFTModule : public ProcessingModule
    {
    public:
        enum class FilterKind
        {
            Smooth = 0,
            Hard = 1
        };

        enum class OutputMode
        {
            Spectrum = 0,
            BandpassSpectrum = 1,
            BandpassImage = 2
        };

        ProcessingModuleKind kind() const noexcept override { return ProcessingModuleKind::FFT; }
        QString name() const override { return "FFT"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        const cv::Mat& maskForSize(const cv::Size& size);
        void invalidateMask();

        double m_minFeatureSize{2.0};
        double m_maxFeatureSize{10.0};
        FilterKind m_filterKind{FilterKind::Smooth};
        OutputMode m_outputMode{OutputMode::BandpassImage};

        cv::Mat m_mask;
        cv::Size m_maskSize;
        double m_maskMinFeatureSize{-1.0};
        double m_maskMaxFeatureSize{-1.0};
        FilterKind m_maskFilterKind{FilterKind::Smooth};

        cv::Mat m_grayFloat;
        cv::Mat m_padded;
        cv::Mat m_complex;
        cv::Mat m_planes[2];
        cv::Mat m_filteredComplex;
        cv::Mat m_filtered;
        cv::Mat m_spectrumMagnitude;
        cv::Mat m_shiftedSpectrum;
    };
}
