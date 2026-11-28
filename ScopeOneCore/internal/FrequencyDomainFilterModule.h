#pragma once

#include "internal/ProcessingModule.h"

#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    class FrequencyDomainFilterModule final : public ProcessingModule
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
            FilteredSpectrum = 1,
            FilteredImage = 2
        };

        // The library that transforms the filtered image; the spectrum outputs use OpenCV
        enum class FftLibrary
        {
            OpenCV = 0,
            Pocketfft = 1
        };

        // Complex: the full complex DFT. Packed: the real DFT of OpenCV (CCS) in one real matrix.
        // Half: the columns kx = 0 .. width / 2 of the real DFT of pocketfft.
        enum class MaskLayout
        {
            Complex,
            Packed,
            Half
        };

        QString id() const override { return QStringLiteral("frequency_domain_filter"); }
        QString name() const override { return QStringLiteral("Frequency Domain Filter"); }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& parameters) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        // size is the size of the transformed image
        const cv::Mat& maskForSize(const cv::Size& size, MaskLayout layout);
        ProcessingResult filterWithPocketfft(const ImageFrame& workingFrame);
        void invalidateMask();

        double m_minFeatureSize{2.0};
        double m_maxFeatureSize{10.0};
        FilterKind m_filterKind{FilterKind::Smooth};
        OutputMode m_outputMode{OutputMode::FilteredImage};
        FftLibrary m_fftLibrary{FftLibrary::Pocketfft};

        cv::Mat m_mask;
        cv::Size m_maskSize;
        double m_maskMinFeatureSize{-1.0};
        double m_maskMaxFeatureSize{-1.0};
        FilterKind m_maskFilterKind{FilterKind::Smooth};
        MaskLayout m_maskLayout{MaskLayout::Complex};
        cv::Mat m_grayFloat;
        cv::Mat m_padded;
        cv::Mat m_complex;
        cv::Mat m_planes[2];
        cv::Mat m_packed;
        cv::Mat m_halfSpectrum;
        cv::Mat m_filtered;
        cv::Mat m_spectrumMagnitude;
        cv::Mat m_shiftedSpectrum;
    };
}
