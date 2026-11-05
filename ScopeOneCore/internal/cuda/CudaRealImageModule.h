#pragma once

#include "scopeone/ProcessingTypes.h"
#include "internal/cuda/GpuRealFrame.h"

namespace scopeone::core::internal::cuda
{
    class CudaRealImageModule
        : public scopeone::core::ProcessingModule
    {
    public:
        explicit CudaRealImageModule(GpuMemoryLayout layout = GpuMemoryLayout::Pitched2D);
        ~CudaRealImageModule() override;

        scopeone::core::ProcessingResult process(
            const scopeone::core::ImageFrame& frame,
            int processingBitDepth) final override;

    protected:
        virtual bool processDevice(const GpuRealFrame& input,
                                   GpuRealFrame& output,
                                   int bitDepth) = 0;

        GpuRealFrame m_inputBuffer;
        GpuRealFrame m_outputBuffer;
        GpuMemoryLayout m_layout;
    };
}
