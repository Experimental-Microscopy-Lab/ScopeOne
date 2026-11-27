#pragma once

#include <QByteArray>

#include <array>
#include <cstring>

namespace scopeone::core::internal
{
    // Copies frames into buffers that no frame holds any more, so the memory of a frame is reused
    // instead of allocated again. A new large allocation is mapped page by page on first touch,
    // which made a copy of a 2048 x 2048 frame take five times as long as one into used memory.
    // A buffer is free when only the pool refers to it; one that a consumer still holds is left
    // alone, so a frame never changes under the code that has it. Used by one thread at a time.
    class FrameBufferPool
    {
    public:
        QByteArray copy(const void* data, qsizetype size)
        {
            for (QByteArray& buffer : m_buffers)
            {
                if (buffer.size() == size && buffer.isDetached())
                {
                    std::memcpy(buffer.data(), data, static_cast<std::size_t>(size));
                    return buffer;
                }
            }
            QByteArray& replaced = m_buffers[m_next];
            m_next = (m_next + 1) % m_buffers.size();
            replaced = QByteArray(static_cast<const char*>(data), size);
            return replaced;
        }

    private:
        std::array<QByteArray, 4> m_buffers;
        std::size_t m_next{0};
    };
}
