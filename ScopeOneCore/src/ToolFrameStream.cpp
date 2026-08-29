#include "scopeone/ToolFrameStream.h"
#include "scopeone/ScopeOneCore.h"

namespace scopeone::ui
{
    ScopeOneToolFrameStream::ScopeOneToolFrameStream(scopeone::core::ScopeOneCore& core,
                                                     QObject* parent)
        : QObject(parent), m_core(core)
    {
        connect(&m_core, &scopeone::core::ScopeOneCore::previewRawFrameReady,
                this, &ScopeOneToolFrameStream::acceptFrame);
    }

    void ScopeOneToolFrameStream::setSourceId(const QString& cameraId)
    {
        m_sourceId = cameraId.trimmed();
    }

    void ScopeOneToolFrameStream::setEnabled(bool enabled)
    {
        m_enabled = enabled;
        if (!m_enabled)
        {
            clearPendingFrame();
        }
    }

    void ScopeOneToolFrameStream::setProcessing(bool processing)
    {
        if (m_processing == processing)
        {
            return;
        }
        m_processing = processing;
        if (!m_processing && m_pendingFrame.isValid())
        {
            const auto frame = m_pendingFrame;
            m_pendingFrame = {};
            m_processing = true;
            emit frameReady(frame);
        }
    }

    void ScopeOneToolFrameStream::clearPendingFrame()
    {
        m_pendingFrame = {};
    }

    void ScopeOneToolFrameStream::acceptFrame(const scopeone::core::ImageFrame& frame)
    {
        if (!m_enabled || !frame.isValid()
            || (!m_sourceId.isEmpty() && frame.cameraId != m_sourceId))
        {
            return;
        }
        if (m_processing)
        {
            m_pendingFrame = frame;
            return;
        }
        m_processing = true;
        emit frameReady(frame);
    }

}
