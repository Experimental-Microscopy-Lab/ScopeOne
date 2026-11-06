#pragma once

#include "scopeone/ImageFrame.h"

#include <cstddef>

namespace scopeone::core::internal::cuda
{
    enum class GpuMemoryLayout
    {
        Pitched2D,
        Contiguous
    };

    class GpuImage
    {
    public:
        GpuImage() = default;
        GpuImage(int width, int height, GpuMemoryLayout layout);
        ~GpuImage();

        GpuImage(const GpuImage&) = delete;
        GpuImage& operator=(const GpuImage&) = delete;
        GpuImage(GpuImage&& other) noexcept;
        GpuImage& operator=(GpuImage&& other) noexcept;

        bool allocate(int width, int height, GpuMemoryLayout layout);
        void release();
        bool isValid() const;

        void* data() const;
        std::size_t pitchBytes() const;
        int width() const;
        int height() const;
        GpuMemoryLayout layout() const;

        bool upload(const scopeone::core::ImageFrame& frame);
        // Writes value * scale + offset, clamped to the frame's bit depth
        bool download(scopeone::core::ImageFrame& frame, float scale, float offset) const;

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
