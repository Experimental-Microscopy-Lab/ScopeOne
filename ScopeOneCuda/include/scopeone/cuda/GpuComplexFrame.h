#pragma once

#include "scopeone/cuda/CudaExport.h"

#include <cstddef>

namespace scopeone::cuda
{
    class SCOPEONE_CUDA_EXPORT GpuComplexFrame
    {
    public:
        GpuComplexFrame() = default;
        ~GpuComplexFrame();

        GpuComplexFrame(const GpuComplexFrame&) = delete;
        GpuComplexFrame& operator=(const GpuComplexFrame&) = delete;
        GpuComplexFrame(GpuComplexFrame&& other) noexcept;
        GpuComplexFrame& operator=(GpuComplexFrame&& other) noexcept;

        bool allocateForRealImage(int width, int height);
        void release();
        bool isValid() const;

        void* data() const;
        int realWidth() const;
        int spectrumWidth() const;
        int height() const;
        std::size_t elementCount() const;

    private:
        void* m_data{nullptr};
        int m_realWidth{0};
        int m_spectrumWidth{0};
        int m_height{0};
    };
}
