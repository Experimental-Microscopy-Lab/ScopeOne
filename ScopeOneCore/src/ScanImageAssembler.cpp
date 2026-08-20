#include "scopeone/ScanImageAssembler.h"

#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace scopeone::core
{
    namespace
    {
        bool matchesMarker(quint32 value, quint32 markerMask)
        {
            return markerMask != 0 && (value & markerMask) != 0;
        }
    }

    ScanImageAssembler::ScanImageAssembler(const QString& sourceId,
                                           const ScanImageConfig& config)
        : m_sourceId(sourceId.trimmed())
          , m_config(config)
    {
        reset();
    }

    bool ScanImageAssembler::isValid() const
    {
        const qint64 pixelCount = static_cast<qint64>(m_config.width)
            * static_cast<qint64>(m_config.height);
        return !m_sourceId.isEmpty()
            && m_config.enabled
            && m_config.width > 0
            && m_config.height > 0
            && m_config.gain > 0
            && m_config.gain <= (std::numeric_limits<quint16>::max)()
            && pixelCount <= 64LL * 1024LL * 1024LL;
    }

    void ScanImageAssembler::reset()
    {
        m_framePixels.clear();
        m_lineEventTicks.clear();
        m_readyFrames.clear();
        m_nextRow = 0;
        m_frameActive = false;
        m_lineActive = false;
        m_lineStartTick = 0;
        m_lastLineDurationTicks = 0;
        m_tickPeriodSeconds = 0.0;
        if (isValid())
        {
            m_framePixels.resize(m_config.width * m_config.height);
        }
    }

    QList<ImageFrame> ScanImageAssembler::append(const TimestampedEventChunk& chunk)
    {
        m_readyFrames.clear();
        if (!isValid()
            || chunk.sourceId != m_sourceId
            || !chunk.isValid())
        {
            return {};
        }
        m_tickPeriodSeconds = chunk.tickPeriodSeconds;

        qsizetype eventIndex = 0;
        qsizetype markerIndex = 0;
        while (eventIndex < chunk.eventTicks.size()
               || markerIndex < chunk.markerTicks.size())
        {
            if (markerIndex < chunk.markerTicks.size()
                && (eventIndex >= chunk.eventTicks.size()
                    || chunk.markerTicks[markerIndex] <= chunk.eventTicks[eventIndex]))
            {
                handleMarker(chunk.markerTicks[markerIndex],
                             chunk.markerCodes[markerIndex]);
                ++markerIndex;
            }
            else
            {
                appendEvent(chunk.eventTicks[eventIndex]);
                ++eventIndex;
            }
        }

        return std::exchange(m_readyFrames, QList<ImageFrame>{});
    }

    QList<ImageFrame> ScanImageAssembler::finish()
    {
        m_readyFrames.clear();
        if (m_lineActive && m_lastLineDurationTicks > 0)
        {
            finishLine(m_lineStartTick + m_lastLineDurationTicks);
        }
        m_frameActive = false;
        m_lineActive = false;
        m_lineEventTicks.clear();
        return std::exchange(m_readyFrames, QList<ImageFrame>{});
    }

    void ScanImageAssembler::handleMarker(quint64 tick, quint32 code)
    {
        if (matchesMarker(code, m_config.frameStartMarker))
        {
            if (m_frameActive)
            {
                finishLine(tick);
                if (m_frameActive && m_nextRow > 0)
                {
                    finishFrame(tick);
                }
            }
            beginFrame();
        }

        if (!m_frameActive)
        {
            if (m_config.frameStartMarker != 0)
            {
                return;
            }
            beginFrame();
        }
        if (matchesMarker(code, m_config.lineStartMarker))
        {
            if (m_lineActive)
            {
                finishLine(tick);
            }
            beginLine(tick);
        }
        if (matchesMarker(code, m_config.lineEndMarker))
        {
            finishLine(tick);
        }
        if (matchesMarker(code, m_config.frameEndMarker))
        {
            finishLine(tick);
            finishFrame(tick);
        }
    }

    void ScanImageAssembler::beginFrame()
    {
        m_framePixels.fill(0);
        m_lineEventTicks.clear();
        m_nextRow = 0;
        m_frameActive = true;
        m_lineActive = false;
    }

    void ScanImageAssembler::beginLine(quint64 tick)
    {
        if (!m_frameActive || m_nextRow >= m_config.height)
        {
            return;
        }
        m_lineEventTicks.clear();
        m_lineStartTick = tick;
        m_lineActive = true;
    }

    void ScanImageAssembler::finishLine(quint64 tick)
    {
        if (!m_lineActive)
        {
            return;
        }
        if (tick > m_lineStartTick && m_nextRow < m_config.height)
        {
            m_lastLineDurationTicks = tick - m_lineStartTick;
            const bool reverse = m_config.serpentine && (m_nextRow % 2 == 1);
            const long double duration = static_cast<long double>(tick - m_lineStartTick);
            for (quint64 eventTick : std::as_const(m_lineEventTicks))
            {
                if (eventTick < m_lineStartTick || eventTick >= tick)
                {
                    continue;
                }
                const int x = std::min(
                    m_config.width - 1,
                    static_cast<int>(static_cast<long double>(eventTick - m_lineStartTick)
                                     / duration * m_config.width));
                const int outputX = reverse ? m_config.width - 1 - x : x;
                quint16& pixel = m_framePixels[m_nextRow * m_config.width + outputX];
                const quint32 amplified = static_cast<quint32>(pixel) + m_config.gain;
                pixel = static_cast<quint16>((std::min)(
                    amplified,
                    static_cast<quint32>((std::numeric_limits<quint16>::max)())));
            }
            ++m_nextRow;
        }
        m_lineEventTicks.clear();
        m_lineActive = false;
        if (m_nextRow >= m_config.height)
        {
            finishFrame(tick);
        }
    }

    void ScanImageAssembler::finishFrame(quint64 tick)
    {
        if (!m_frameActive || m_nextRow <= 0)
        {
            m_frameActive = false;
            m_lineActive = false;
            return;
        }

        const qint64 byteCount = static_cast<qint64>(m_config.width)
            * static_cast<qint64>(m_config.height) * sizeof(quint16);
        if (byteCount <= 0 || byteCount > (std::numeric_limits<int>::max)())
        {
            reset();
            return;
        }
        ImageFrame frame;
        frame.cameraId = m_sourceId;
        frame.width = m_config.width;
        frame.height = m_config.height;
        frame.stride = m_config.width * static_cast<int>(sizeof(quint16));
        frame.bitsPerSample = 16;
        frame.pixelFormat = ImagePixelFormat::Mono16;
        frame.frameIndex = m_nextFrameIndex++;
        frame.timestampNs = m_tickPeriodSeconds > 0.0
                                ? static_cast<quint64>(
                                      static_cast<double>(tick)
                                      * m_tickPeriodSeconds * 1.0e9)
                                : 0;
        frame.bytes.resize(static_cast<int>(byteCount));
        std::memcpy(frame.bytes.data(),
                    m_framePixels.constData(),
                    static_cast<size_t>(byteCount));
        m_readyFrames.append(std::move(frame));
        m_frameActive = false;
        m_lineActive = false;
        m_lineEventTicks.clear();
    }

    void ScanImageAssembler::appendEvent(quint64 tick)
    {
        if (!m_frameActive)
        {
            if (m_config.frameStartMarker != 0)
            {
                return;
            }
            beginFrame();
        }
        if (!m_lineActive)
        {
            if (m_config.lineStartMarker != 0)
            {
                return;
            }
            beginLine(tick);
        }
        if (!m_lineActive)
        {
            return;
        }
        m_lineEventTicks.append(tick);
    }
}
