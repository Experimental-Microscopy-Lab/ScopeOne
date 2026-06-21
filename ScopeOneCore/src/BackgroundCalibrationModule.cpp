#include "internal/BackgroundCalibrationModule.h"
#include "internal/FrameBufferUtils.h"

#include <QMutexLocker>

#include <algorithm>

namespace scopeone::core::internal
{
    namespace
    {
        QString frameHistoryKey(const ImageFrame& frame)
        {
            return frame.cameraId.isEmpty() ? QStringLiteral("__default__") : frame.cameraId;
        }
    }

    BackgroundCalibrationModule::BackgroundCalibrationModule(QObject* parent)
        : ProcessingModule(parent)
          , m_calibrationFrames(101)
          , m_operation(BackgroundOperation::Subtract)
          , m_method(BackgroundMethod::Median)
    {
    }

    void BackgroundCalibrationModule::resetCalibration()
    {
        QMutexLocker locker(&m_mutex);
        m_states.clear();
    }

    ImageFrame BackgroundCalibrationModule::computeBackground(const std::deque<ImageFrame>& buffer) const
    {
        if (buffer.empty())
        {
            return {};
        }

        const int w = buffer.front().width;
        const int h = buffer.front().height;
        const int maxValue = buffer.front().maxValue();
        QByteArray bgBytes = dispatchFrameType(buffer.front(), [&]<typename Pixel>()
        {
            QByteArray outBytes = allocatePixelBytes<Pixel>(w, h);
            switch (m_method)
            {
            case BackgroundMethod::Median:
                {
                    std::vector<Pixel> vals(buffer.size());
                    for (int y = 0; y < h; ++y)
                    {
                        Pixel* dst = mutableRowData<Pixel>(outBytes, w, y);
                        for (int x = 0; x < w; ++x)
                        {
                            for (size_t k = 0; k < buffer.size(); ++k)
                            {
                                vals[k] = frameRowData<Pixel>(buffer[k], y)[x];
                            }
                            std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                            dst[x] = vals[vals.size() / 2];
                        }
                    }
                    break;
                }
            case BackgroundMethod::Mean:
                {
                    for (int y = 0; y < h; ++y)
                    {
                        Pixel* dst = mutableRowData<Pixel>(outBytes, w, y);
                        for (int x = 0; x < w; ++x)
                        {
                            qint64 sum = 0;
                            for (size_t k = 0; k < buffer.size(); ++k)
                            {
                                sum += static_cast<int>(frameRowData<Pixel>(buffer[k], y)[x]);
                            }
                            dst[x] = clampPixelValue<Pixel>(
                                static_cast<int>(sum / static_cast<qint64>(buffer.size())),
                                maxValue);
                        }
                    }
                    break;
                }
            case BackgroundMethod::Maximum:
                {
                    for (int y = 0; y < h; ++y)
                    {
                        Pixel* dst = mutableRowData<Pixel>(outBytes, w, y);
                        for (int x = 0; x < w; ++x)
                        {
                            int best = 0;
                            for (size_t k = 0; k < buffer.size(); ++k)
                            {
                                best = qMax(best, static_cast<int>(frameRowData<Pixel>(buffer[k], y)[x]));
                            }
                            dst[x] = clampPixelValue<Pixel>(best, maxValue);
                        }
                    }
                    break;
                }
            case BackgroundMethod::Minimum:
                {
                    for (int y = 0; y < h; ++y)
                    {
                        Pixel* dst = mutableRowData<Pixel>(outBytes, w, y);
                        for (int x = 0; x < w; ++x)
                        {
                            int best = maxValue;
                            for (size_t k = 0; k < buffer.size(); ++k)
                            {
                                best = qMin(best, static_cast<int>(frameRowData<Pixel>(buffer[k], y)[x]));
                            }
                            dst[x] = clampPixelValue<Pixel>(best, maxValue);
                        }
                    }
                    break;
                }
            }
            return outBytes;
        });

        return makeFrameLike(buffer.front(), w, h, std::move(bgBytes));
    }

    bool BackgroundCalibrationModule::process(const ModuleInput& in, ModuleOutput& out)
    {
        if (!in.frame.isValid())
        {
            out.frame = in.frame;
            out.error = "Invalid input";
            return false;
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth))
            {
                out.frame = in.frame;
                out.error = "Unsupported input frame";
                return false;
            }

            ImageFrame background;
            BackgroundOperation operation = BackgroundOperation::Subtract;
            {
                QMutexLocker locker(&m_mutex);
                CameraState& state = m_states[frameHistoryKey(in.frame)];
                if ((!state.buffer.empty() && !state.buffer.front().isCompatibleWith(workingFrame))
                    || (state.background.isValid() && !state.background.isCompatibleWith(workingFrame)))
                {
                    state = CameraState{};
                }

                if (m_mode == BackgroundMode::Running)
                {
                    if ((int)state.buffer.size() == m_calibrationFrames)
                    {
                        state.background = computeBackground(state.buffer);
                    }
                    state.buffer.push_back(workingFrame);
                    if ((int)state.buffer.size() > m_calibrationFrames)
                    {
                        state.buffer.pop_front();
                    }
                    if (!state.background.isValid())
                    {
                        out.frame = in.frame;
                        return true;
                    }
                }
                else if (!state.calibrated)
                {
                    state.buffer.push_back(workingFrame);
                    if ((int)state.buffer.size() > m_calibrationFrames)
                    {
                        state.buffer.pop_front();
                    }
                    if ((int)state.buffer.size() < m_calibrationFrames)
                    {
                        out.frame = in.frame;
                        return true;
                    }

                    state.background = computeBackground(state.buffer);
                    state.buffer.clear();
                    state.calibrated = true;
                }

                background = state.background;
                operation = m_operation;
            }

            if (background.isValid() && background.isCompatibleWith(workingFrame))
            {
                const int maxValue = workingFrame.maxValue();
                QByteArray outBytes = dispatchFrameType(workingFrame, [&]<typename Pixel>()
                {
                    QByteArray bytes = allocatePixelBytes<Pixel>(workingFrame.width, workingFrame.height);
                    for (int y = 0; y < workingFrame.height; ++y)
                    {
                        const Pixel* s = frameRowData<Pixel>(workingFrame, y);
                        const Pixel* b = frameRowData<Pixel>(background, y);
                        Pixel* d = mutableRowData<Pixel>(bytes, workingFrame.width, y);
                        for (int x = 0; x < workingFrame.width; ++x)
                        {
                            int result = 0;
                            switch (operation)
                            {
                            case BackgroundOperation::Subtract:
                                result = int(s[x]) - int(b[x]);
                                break;
                            case BackgroundOperation::Add:
                                result = int(s[x]) + int(b[x]);
                                break;
                            case BackgroundOperation::Multiply:
                                result = static_cast<int>((static_cast<qint64>(s[x]) * b[x]) / maxValue);
                                break;
                            case BackgroundOperation::Divide:
                                result = (b[x] > 0)
                                             ? static_cast<int>((static_cast<qint64>(s[x]) * maxValue) / b[x])
                                             : maxValue;
                                break;
                            }
                            d[x] = clampPixelValue<Pixel>(result, maxValue);
                        }
                    }
                    return bytes;
                });

                out.frame = makeFrameLike(workingFrame,
                                          workingFrame.width,
                                          workingFrame.height,
                                          std::move(outBytes));
            }
            else
            {
                out.frame = workingFrame;
            }
        }
        catch (const std::exception& e)
        {
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
        QMutexLocker locker(&m_mutex);
        bool needsReset = false;

        if (params.contains("calibration_frames"))
        {
            int n = params["calibration_frames"].toInt();
            if (n < 3) n = 3;
            if (n % 2 == 0) n += 1;
            if (n != m_calibrationFrames)
            {
                m_calibrationFrames = n;
                needsReset = true;
            }
        }

        if (params.contains("method"))
        {
            int methodInt = params["method"].toInt();
            if (methodInt >= 0 && methodInt <= 3)
            {
                BackgroundMethod newMethod = static_cast<BackgroundMethod>(methodInt);
                if (newMethod != m_method)
                {
                    m_method = newMethod;
                    needsReset = true;
                }
            }
        }

        if (params.contains("operation"))
        {
            int opInt = params["operation"].toInt();
            if (opInt >= 0 && opInt <= 3)
            {
                BackgroundOperation newOp = static_cast<BackgroundOperation>(opInt);
                if (newOp != m_operation)
                {
                    m_operation = newOp;
                }
            }
        }

        if (params.contains("mode"))
        {
            int modeInt = params["mode"].toInt();
            if (modeInt >= 0 && modeInt <= 1)
            {
                BackgroundMode newMode = static_cast<BackgroundMode>(modeInt);
                if (newMode != m_mode)
                {
                    m_mode = newMode;
                    needsReset = true;
                }
            }
        }

        if (needsReset)
        {
            m_states.clear();
        }
    }
} // namespace scopeone::core::internal
