#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/cuda/CudaExport.h"

#include <cstddef>

namespace scopeone::cuda
{
    enum class GpuMemoryLayout
    {
        Pitched2D,
        Contiguous
    };

    class SCOPEONE_CUDA_EXPORT GpuRealFrame
    {
    public:
        GpuRealFrame() = default;
        GpuRealFrame(int width, int height, GpuMemoryLayout layout);
        ~GpuRealFrame();

        GpuRealFrame(const GpuRealFrame&) = delete;
        GpuRealFrame& operator=(const GpuRealFrame&) = delete;
        GpuRealFrame(GpuRealFrame&& other) noexcept;
        GpuRealFrame& operator=(GpuRealFrame&& other) noexcept;

        bool allocate(int width, int height, GpuMemoryLayout layout);
        void release();
        bool isValid() const;

        void* data() const;
        std::size_t pitchBytes() const;
        int width() const;
        int height() const;
        GpuMemoryLayout layout() const;

        bool upload(const scopeone::core::ImageFrame& frame);
        bool download(scopeone::core::ImageFrame& frame) const;

    private:
        bool ensureUploadBuffer(std::size_t bytes);
        bool ensureDownloadBuffer(std::size_t bytes) const;

        float* m_data{nullptr};
        std::size_t m_pitchBytes{0};
        int m_width{0};
        int m_height{0};
        GpuMemoryLayout m_layout{GpuMemoryLayout::Pitched2D};
        unsigned char* m_uploadBuffer{nullptr};
        std::size_t m_uploadCapacity{0};
        mutable unsigned char* m_downloadBuffer{nullptr};
        mutable std::size_t m_downloadCapacity{0};
    };
}
