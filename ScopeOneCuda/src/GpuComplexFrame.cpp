#include "scopeone/cuda/GpuComplexFrame.h"

#include <cuda_runtime.h>

#include <utility>

namespace scopeone::cuda
{
    GpuComplexFrame::~GpuComplexFrame()
    {
        release();
    }

    GpuComplexFrame::GpuComplexFrame(GpuComplexFrame&& other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_realWidth(std::exchange(other.m_realWidth, 0))
        , m_spectrumWidth(std::exchange(other.m_spectrumWidth, 0))
        , m_height(std::exchange(other.m_height, 0))
    {
    }

    GpuComplexFrame& GpuComplexFrame::operator=(GpuComplexFrame&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_realWidth = std::exchange(other.m_realWidth, 0);
            m_spectrumWidth = std::exchange(other.m_spectrumWidth, 0);
            m_height = std::exchange(other.m_height, 0);
        }
        return *this;
    }

    bool GpuComplexFrame::allocateForRealImage(int width, int height)
    {
        if (isValid() && m_realWidth == width && m_height == height)
        {
            return true;
        }

        release();
        m_realWidth = width;
        m_spectrumWidth = width / 2 + 1;
        m_height = height;
        const std::size_t bytes = static_cast<std::size_t>(m_spectrumWidth)
            * static_cast<std::size_t>(m_height)
            * sizeof(float2);
        if (cudaMalloc(&m_data, bytes) != cudaSuccess)
        {
            release();
            return false;
        }
        return true;
    }

    void GpuComplexFrame::release()
    {
        if (m_data)
        {
            cudaFree(m_data);
        }
        m_data = nullptr;
        m_realWidth = 0;
        m_spectrumWidth = 0;
        m_height = 0;
    }

    bool GpuComplexFrame::isValid() const
    {
        return m_data != nullptr
            && m_realWidth > 0
            && m_spectrumWidth > 0
            && m_height > 0;
    }

    void* GpuComplexFrame::data() const
    {
        return m_data;
    }

    int GpuComplexFrame::realWidth() const
    {
        return m_realWidth;
    }

    int GpuComplexFrame::spectrumWidth() const
    {
        return m_spectrumWidth;
    }

    int GpuComplexFrame::height() const
    {
        return m_height;
    }

    std::size_t GpuComplexFrame::elementCount() const
    {
        return static_cast<std::size_t>(m_spectrumWidth)
            * static_cast<std::size_t>(m_height);
    }
}
