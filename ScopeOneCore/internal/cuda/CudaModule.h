#pragma once

#include "scopeone/ProcessingTypes.h"
#include "internal/cuda/GpuImage.h"

namespace scopeone::core::internal::cuda
{
    struct GpuImageSize
    {
        int width{0};
        int height{0};
    };

    // The output pixel is value * scale + offset, clamped to the processing bit depth
    struct GpuOutputMapping
    {
        float scale{1.0f};
        float offset{0.0f};
    };

    class CudaModule
        : public scopeone::core::ProcessingModule
    {
    public:
        explicit CudaModule(GpuMemoryLayout layout = GpuMemoryLayout::Pitched2D);
        ~CudaModule() override;

        scopeone::core::ProcessingResult process(
            const scopeone::core::ImageFrame& frame,
            int processingBitDepth) final override;

    protected:
        // The output image is allocated with outputSize and has the input's layout
        virtual bool processDevice(const GpuImage& input,
                                   GpuImage& output,
                                   int bitDepth) = 0;

        // A method that bins, crops or scales returns the size of its result
        virtual GpuImageSize outputSize(int inputWidth, int inputHeight) const
        {
            return {inputWidth, inputHeight};
        }

        // A method with a signed result, such as a difference of Gaussians, moves zero to the
        // middle of the range with an offset of half the maximum value
        virtual GpuOutputMapping outputMapping(int bitDepth) const
        {
            Q_UNUSED(bitDepth);
            return {};
        }

        GpuImage m_inputBuffer;
        GpuImage m_outputBuffer;
        GpuMemoryLayout m_layout;
    };
}
