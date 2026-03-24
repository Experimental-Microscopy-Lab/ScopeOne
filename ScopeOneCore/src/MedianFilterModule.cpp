#include "internal/MedianFilterModule.h"
#include "internal/FrameBufferUtils.h"
#include <QDebug>
#include <algorithm>

namespace scopeone::core::internal {

MedianFilterModule::MedianFilterModule(QObject* parent)
    : ProcessingModule(parent)
    , m_windowSize(5)
{
}

bool MedianFilterModule::process(const ModuleInput& in, ModuleOutput& out)
{
    // Keep a rolling window for temporal median filtering
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame mono8Frame;
        if (!convertFrameToMono8(in.frame, mono8Frame)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        m_frameBuffer.push_back(mono8Frame);
        while ((int)m_frameBuffer.size() > m_windowSize) {
            m_frameBuffer.pop_front();
        }

        if ((int)m_frameBuffer.size() < m_windowSize) {
            out.frame = in.frame;
        } else {
            const int w = mono8Frame.width;
            const int h = mono8Frame.height;
            QByteArray bytes;
            bytes.resize(w * h);
            uchar* outData = reinterpret_cast<uchar*>(bytes.data());
            std::vector<uchar> vals(m_frameBuffer.size());
            for (int y = 0; y < h; ++y) {
                uchar* dstRow = outData + y * w;
                for (int x = 0; x < w; ++x) {
                    for (size_t k = 0; k < m_frameBuffer.size(); ++k) {
                        vals[k] = static_cast<uchar>(
                            reinterpret_cast<const uchar*>(
                                m_frameBuffer[k].bytes.constData() + y * m_frameBuffer[k].stride)[x]);
                    }
                    std::nth_element(vals.begin(), vals.begin() + vals.size()/2, vals.end());
                    dstRow[x] = vals[vals.size()/2];
                }
            }

            out.frame = makeMono8Frame(in.frame.cameraId, w, h, std::move(bytes));
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
    // Clear buffered history when the window changes
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
    // Drop buffered frames for a clean restart
    m_frameBuffer.clear();
}

} // namespace scopeone::core::internal
