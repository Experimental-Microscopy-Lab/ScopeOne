#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

#include <atomic>
#include <functional>
#include <utility>

namespace scopeone::core
{
    enum class DhmRoiMode
    {
        FullFrame,
        CenterCrop
    };

    enum class DhmOutputMode
    {
        QuantitativePhase,
        WrappedPhase,
        Amplitude,
        Spectrum
    };

    struct DhmParameters
    {
        int sidebandX{48};
        int sidebandY{-32};
        int radius{24};
        bool autoDetectSideband{true};
        bool softEdge{true};
        double softEdgeSigma{2.0};
        double wavelength{632.8e-9};
        double pixelSize{5.5e-6};
        double z{0.0};
        bool unwrapPhase{false};
        bool removeTilt{false};
        DhmRoiMode roiMode{DhmRoiMode::FullFrame};
        int roiSize{512};
        DhmOutputMode outputMode{DhmOutputMode::QuantitativePhase};
    };

    struct DhmResult
    {
        scopeone::core::ImageFrame outputFrame;
        scopeone::core::ImageFrame spectrumFrame;
        int detectedSidebandX{0};
        int detectedSidebandY{0};
    };

    // Reconstructs an off axis hologram. The output and the spectrum are 16-bit frames
    SCOPEONE_CORE_EXPORT DhmResult reconstructDhm(
        const ImageFrame& input,
        const DhmParameters& params,
        const std::atomic_bool& cancel,
        const std::function<void(int)>& progress);
}
