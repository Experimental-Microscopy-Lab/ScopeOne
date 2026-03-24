#include "internal/BackgroundCalibrationModule.h"
#include "internal/FrameBufferUtils.h"
#include <QDebug>
#include <algorithm>

namespace scopeone::core::internal {

BackgroundCalibrationModule::BackgroundCalibrationModule(QObject* parent)
    : ProcessingModule(parent)
    , m_calibrationFrames(101)
    , m_calibrated(false)
    , m_operation(BackgroundOperation::Subtract)
    , m_method(BackgroundMethod::Median)
{
}

void BackgroundCalibrationModule::resetCalibration()
{
    // Drop the current background model
    m_buffer.clear();
    m_background = ImageFrame{};
    m_calibrated = false;
}

void BackgroundCalibrationModule::computeBackground()
{
    // Rebuild the background frame from buffered samples
    if (m_buffer.empty()) return;

    const int w = m_buffer.front().width;
    const int h = m_buffer.front().height;
    QByteArray bgBytes;
    bgBytes.resize(w * h);
    uchar* bgData = reinterpret_cast<uchar*>(bgBytes.data());

    switch (m_method) {
        case BackgroundMethod::Median: {
            std::vector<uchar> vals(m_buffer.size());
            for (int y = 0; y < h; ++y) {
                uchar* dst = bgData + y * w;
                for (int x = 0; x < w; ++x) {
                    for (size_t k = 0; k < m_buffer.size(); ++k) {
                        vals[k] = static_cast<uchar>(
                            reinterpret_cast<const uchar*>(
                                m_buffer[k].bytes.constData() + y * m_buffer[k].stride)[x]);
                    }
                    std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                    dst[x] = vals[vals.size() / 2];
                }
            }
            break;
        }

        case BackgroundMethod::Mean: {
            for (int y = 0; y < h; ++y) {
                uchar* dst = bgData + y * w;
                for (int x = 0; x < w; ++x) {
                    int sum = 0;
                    for (size_t k = 0; k < m_buffer.size(); ++k) {
                        sum += reinterpret_cast<const uchar*>(
                            m_buffer[k].bytes.constData() + y * m_buffer[k].stride)[x];
                    }
                    dst[x] = static_cast<uchar>(sum / m_buffer.size());
                }
            }
            break;
        }

        case BackgroundMethod::Maximum: {
            for (int y = 0; y < h; ++y) {
                uchar* dst = bgData + y * w;
                for (int x = 0; x < w; ++x) {
                    uchar maxVal = 0;
                    for (size_t k = 0; k < m_buffer.size(); ++k) {
                        uchar val = reinterpret_cast<const uchar*>(
                            m_buffer[k].bytes.constData() + y * m_buffer[k].stride)[x];
                        if (val > maxVal) maxVal = val;
                    }
                    dst[x] = maxVal;
                }
            }
            break;
        }

        case BackgroundMethod::Minimum: {
            for (int y = 0; y < h; ++y) {
                uchar* dst = bgData + y * w;
                for (int x = 0; x < w; ++x) {
                    uchar minVal = 255;
                    for (size_t k = 0; k < m_buffer.size(); ++k) {
                        uchar val = reinterpret_cast<const uchar*>(
                            m_buffer[k].bytes.constData() + y * m_buffer[k].stride)[x];
                        if (val < minVal) minVal = val;
                    }
                    dst[x] = minVal;
                }
            }
            break;
        }
    }

    m_background = makeMono8Frame(m_buffer.front().cameraId, w, h, std::move(bgBytes));
}

bool BackgroundCalibrationModule::process(const ModuleInput& in, ModuleOutput& out)
{
    // Learn or apply the background model on mono frames
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame gray;
        if (!convertFrameToMono8(in.frame, gray)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        if (m_mode == BackgroundMode::Snapshot) {
            if (!m_calibrated) {
                m_buffer.push_back(gray);
                while ((int)m_buffer.size() > m_calibrationFrames) m_buffer.pop_front();

                if ((int)m_buffer.size() < m_calibrationFrames) {
                    out.frame = in.frame;
                } else {
                    computeBackground();
                    m_calibrated = true;

                    out.frame = in.frame;
                }
            }
        } else {
            computeBackground();
        }

        if (m_mode == BackgroundMode::Running) {
            m_buffer.push_back(gray);
            while ((int)m_buffer.size() > m_calibrationFrames) {
                m_buffer.pop_front();
            }
        }

        if (m_background.isValid() && m_background.isCompatibleWith(gray)) {
            QByteArray outBytes;
            outBytes.resize(gray.width * gray.height);
            uchar* outData = reinterpret_cast<uchar*>(outBytes.data());

            for (int y = 0; y < gray.height; ++y) {
                const uchar* s = reinterpret_cast<const uchar*>(gray.bytes.constData() + y * gray.stride);
                const uchar* b = reinterpret_cast<const uchar*>(m_background.bytes.constData()
                                                                + y * m_background.stride);
                uchar* d = outData + y * gray.width;

                for (int x = 0; x < gray.width; ++x) {
                    int result = 0;

                    switch (m_operation) {
                        case BackgroundOperation::Subtract:
                            result = int(s[x]) - int(b[x]);
                            break;
                        case BackgroundOperation::Add:
                            result = int(s[x]) + int(b[x]);
                            break;
                        case BackgroundOperation::Multiply:
                            result = (int(s[x]) * int(b[x])) / 255;
                            break;
                        case BackgroundOperation::Divide:
                            result = (b[x] > 0) ? (int(s[x]) * 255) / int(b[x]) : 255;
                            break;
                    }

                    d[x] = static_cast<uchar>(qBound(0, result, 255));
                }
            }

            out.frame = makeMono8Frame(in.frame.cameraId, gray.width, gray.height, std::move(outBytes));
        } else {
            out.frame = gray;
        }

    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Background calibration failed: %1").arg(e.what());
        return false;
    }

    return true;
}

QVariantMap BackgroundCalibrationModule::getParameters() const
{
    QVariantMap p;
    p["calibration_frames"] = m_calibrationFrames;
    p["operation"] = static_cast<int>(m_operation);
    p["method"] = static_cast<int>(m_method);
    p["mode"] = static_cast<int>(m_mode);
    return p;
}

void BackgroundCalibrationModule::setParameters(const QVariantMap& params)
{
    // Reset calibration when model settings change
    bool needsReset = false;

    if (params.contains("calibration_frames")) {
        int n = params["calibration_frames"].toInt();
        if (n < 3) n = 3;
        if (n % 2 == 0) n += 1;
        if (n != m_calibrationFrames) {
            m_calibrationFrames = n;
            needsReset = true;
        }
    }

    if (params.contains("method")) {
        int methodInt = params["method"].toInt();
        if (methodInt >= 0 && methodInt <= 3) {
            BackgroundMethod newMethod = static_cast<BackgroundMethod>(methodInt);
            if (newMethod != m_method) {
                m_method = newMethod;
                needsReset = true;
            }
        }
    }

    if (params.contains("operation")) {
        int opInt = params["operation"].toInt();
        if (opInt >= 0 && opInt <= 3) {
            BackgroundOperation newOp = static_cast<BackgroundOperation>(opInt);
            if (newOp != m_operation) {
                m_operation = newOp;
            }
        }
    }

    if (params.contains("mode")) {
        int modeInt = params["mode"].toInt();
        if (modeInt >= 0 && modeInt <= 1) {
            BackgroundMode newMode = static_cast<BackgroundMode>(modeInt);
            if (newMode != m_mode) {
                m_mode = newMode;
                needsReset = true;
            }
        }
    }

    if (needsReset) {
        resetCalibration();
    }
}

} // namespace scopeone::core::internal
