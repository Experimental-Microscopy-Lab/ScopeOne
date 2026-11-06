#include "internal/cuda/GpuImage.h"

#include "internal/cuda/CudaKernelLaunch.h"

#include <cuda_runtime.h>

#include <utility>

namespace scopeone::core::internal::cuda
{
    GpuImage::GpuImage(int width, int height, GpuMemoryLayout layout)
    {
        allocate(width, height, layout);
    }

    GpuImage::~GpuImage()
    {
        release();
    }

    GpuImage::GpuImage(GpuImage&& other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_pitchBytes(std::exchange(other.m_pitchBytes, 0))
        , m_width(std::exchange(other.m_width, 0))
        , m_height(std::exchange(other.m_height, 0))
        , m_layout(other.m_layout)
        , m_uploadBuffer(std::exchange(other.m_uploadBuffer, nullptr))
        , m_uploadCapacity(std::exchange(other.m_uploadCapacity, 0))
        , m_downloadBuffer(std::exchange(other.m_downloadBuffer, nullptr))
        , m_downloadCapacity(std::exchange(other.m_downloadCapacity, 0))
    {
    }

    GpuImage& GpuImage::operator=(GpuImage&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_pitchBytes = std::exchange(other.m_pitchBytes, 0);
            m_width = std::exchange(other.m_width, 0);
            m_height = std::exchange(other.m_height, 0);
            m_layout = other.m_layout;
            m_uploadBuffer = std::exchange(other.m_uploadBuffer, nullptr);
            m_uploadCapacity = std::exchange(other.m_uploadCapacity, 0);
            m_downloadBuffer = std::exchange(other.m_downloadBuffer, nullptr);
            m_downloadCapacity = std::exchange(other.m_downloadCapacity, 0);
        }
        return *this;
    }

    bool GpuImage::allocate(int width, int height, GpuMemoryLayout layout)
    {
        if (isValid() && m_width == width && m_height == height && m_layout == layout)
        {
            return true;
        }

        release();
        m_width = width;
        m_height = height;
        m_layout = layout;
        if (layout == GpuMemoryLayout::Pitched2D)
        {
            if (cudaMallocPitch(reinterpret_cast<void**>(&m_data),
                                &m_pitchBytes,
                                static_cast<std::size_t>(width) * sizeof(float),
                                height) != cudaSuccess)
            {
                release();
                return false;
            }
        }
        else
        {
            m_pitchBytes = static_cast<std::size_t>(width) * sizeof(float);
            if (cudaMalloc(reinterpret_cast<void**>(&m_data),
                           m_pitchBytes * static_cast<std::size_t>(height)) != cudaSuccess)
            {
                release();
                return false;
            }
        }
        return true;
    }

    void GpuImage::release()
    {
        if (m_data)
        {
            cudaFree(m_data);
        }
        if (m_uploadBuffer)
        {
            cudaFree(m_uploadBuffer);
        }
        if (m_downloadBuffer)
        {
            cudaFree(m_downloadBuffer);
        }
        m_data = nullptr;
        m_pitchBytes = 0;
        m_width = 0;
        m_height = 0;
        m_uploadBuffer = nullptr;
        m_uploadCapacity = 0;
        m_downloadBuffer = nullptr;
        m_downloadCapacity = 0;
    }

    bool GpuImage::isValid() const
    {
        return m_data != nullptr && m_width > 0 && m_height > 0 && m_pitchBytes > 0;
    }

    void* GpuImage::data() const
    {
        return m_data;
    }

    std::size_t GpuImage::pitchBytes() const
    {
        return m_pitchBytes;
    }

    int GpuImage::width() const
    {
        return m_width;
    }

    int GpuImage::height() const
    {
        return m_height;
    }

    GpuMemoryLayout GpuImage::layout() const
    {
        return m_layout;
    }

    bool GpuImage::ensureUploadBuffer(std::size_t bytes)
    {
        if (m_uploadCapacity >= bytes)
        {
            return true;
        }
        if (m_uploadBuffer)
        {
            cudaFree(m_uploadBuffer);
        }
        m_uploadBuffer = nullptr;
        m_uploadCapacity = 0;
        if (cudaMalloc(reinterpret_cast<void**>(&m_uploadBuffer), bytes) != cudaSuccess)
        {
            return false;
        }
        m_uploadCapacity = bytes;
        return true;
    }

    bool GpuImage::ensureDownloadBuffer(std::size_t bytes) const
    {
        if (m_downloadCapacity >= bytes)
        {
            return true;
        }
        if (m_downloadBuffer)
        {
            cudaFree(m_downloadBuffer);
        }
        m_downloadBuffer = nullptr;
        m_downloadCapacity = 0;
        if (cudaMalloc(reinterpret_cast<void**>(&m_downloadBuffer), bytes) != cudaSuccess)
        {
            return false;
        }
        m_downloadCapacity = bytes;
        return true;
    }

    bool GpuImage::upload(const scopeone::core::ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return false;
        }
        if (!isValid()
            || m_width != frame.width
            || m_height != frame.height)
        {
            if (!allocate(frame.width, frame.height, m_layout))
            {
                return false;
            }
        }
        if (!ensureUploadBuffer(static_cast<std::size_t>(frame.payloadByteCount())))
        {
            return false;
        }
        if (cudaMemcpy2D(m_uploadBuffer,
                         static_cast<std::size_t>(frame.stride),
                         frame.bytes.constData(),
                         static_cast<std::size_t>(frame.stride),
                         static_cast<std::size_t>(frame.stride),
                         static_cast<std::size_t>(frame.height),
                         cudaMemcpyHostToDevice) != cudaSuccess)
        {
            return false;
        }
        return detail::convertToFloat(m_uploadBuffer,
                                      frame.stride,
                                      frame.width,
                                      frame.height,
                                      frame.bytesPerPixel(),
                                      m_data,
                                      m_pitchBytes);
    }

    bool GpuImage::download(scopeone::core::ImageFrame& frame, float scale, float offset) const
    {
        if (!isValid()
            || frame.width != m_width
            || frame.height != m_height
            || (frame.pixelFormat != scopeone::core::ImagePixelFormat::Mono8
                && frame.pixelFormat != scopeone::core::ImagePixelFormat::Mono16))
        {
            return false;
        }
        if (!ensureDownloadBuffer(static_cast<std::size_t>(frame.payloadByteCount())))
        {
            return false;
        }
        if (!detail::convertFromFloat(m_data,
                                      m_pitchBytes,
                                      m_downloadBuffer,
                                      frame.stride,
                                      frame.width,
                                      frame.height,
                                      frame.bytesPerPixel(),
                                      scale,
                                      offset))
        {
            return false;
        }
        return cudaMemcpy2D(frame.bytes.data(),
                            static_cast<std::size_t>(frame.stride),
                            m_downloadBuffer,
                            static_cast<std::size_t>(frame.stride),
                            static_cast<std::size_t>(frame.stride),
                            static_cast<std::size_t>(frame.height),
                            cudaMemcpyDeviceToHost) == cudaSuccess;
    }
}
