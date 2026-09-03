#pragma once

#include "scopeone/ProcessingPlugin.h"
#include "scopeone/cuda/CudaExport.h"
#include "scopeone/cuda/GpuRealFrame.h"

namespace scopeone::cuda
{
    class SCOPEONE_CUDA_EXPORT CudaRealImageModule
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
