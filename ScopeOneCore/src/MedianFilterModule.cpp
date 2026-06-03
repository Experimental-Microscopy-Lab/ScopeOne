#include "internal/MedianFilterModule.h"
#include "internal/FrameBufferUtils.h"
#include <algorithm>

namespace scopeone::core::internal {

MedianFilterModule::MedianFilterModule(QObject* parent)
    : ProcessingModule(parent)
    , m_windowSize(5)
{
}

bool MedianFilterModule::process(const ModuleInput& in, ModuleOutput& out)
{
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame workingFrame;
        if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        if (!m_frameBuffer.empty() && !m_frameBuffer.front().isCompatibleWith(workingFrame)) {
            m_frameBuffer.clear();
        }

        m_frameBuffer.push_back(workingFrame);
        while ((int)m_frameBuffer.size() > m_windowSize) {
            m_frameBuffer.pop_front();
        }

        if ((int)m_frameBuffer.size() < m_windowSize) {
            out.frame = in.frame;
        } else {
            const int w = workingFrame.width;
            const int h = workingFrame.height;
            QByteArray bytes = dispatchFrameType(workingFrame, [&]<typename Pixel>()
            {
                QByteArray outBytes = allocatePixelBytes<Pixel>(w, h);
                std::vector<Pixel> vals(m_frameBuffer.size());
                for (int y = 0; y < h; ++y) {
                    Pixel* dstRow = mutableRowData<Pixel>(outBytes, w, y);
                    for (int x = 0; x < w; ++x) {
                        for (size_t k = 0; k < m_frameBuffer.size(); ++k) {
                            vals[k] = frameRowData<Pixel>(m_frameBuffer[k], y)[x];
                        }
                        std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                        dstRow[x] = vals[vals.size() / 2];
                    }
                }
                return outBytes;
            });
            out.frame = makeFrameLike(workingFrame, w, h, std::move(bytes));
        }

    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Temporal median filtering failed: %1").arg(e.what());
        return false;
    }

    return true;
}

QVariantMap MedianFilterModule::getParameters() const
{
    QVariantMap params;
    params["window_size"] = m_windowSize;
    return params;
}

void MedianFilterModule::setParameters(const QVariantMap& params)
{
    if (params.contains("window_size")) {
        int w = params["window_size"].toInt();
        if (w < 3) w = 3;
        if (w % 2 == 0) w += 1;
        if (w != m_windowSize) {
            m_windowSize = w;
            m_frameBuffer.clear();
        }
    }
}

void MedianFilterModule::resetBuffer()
{
    m_frameBuffer.clear();
}

} // namespace scopeone::core::internal
