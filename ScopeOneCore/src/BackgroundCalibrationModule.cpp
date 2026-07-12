#include "internal/BackgroundCalibrationModule.h"
#include "internal/FrameBufferUtils.h"
#include <algorithm>

namespace scopeone::core::internal
{
    // Creates an independent background calibration runtime
    std::unique_ptr<ProcessingModule> BackgroundCalibrationModule::createRuntime() const
    {
        auto module = std::make_unique<BackgroundCalibrationModule>();
        module->setParameters(parameters());
        return module;
    }

    // Clears background calibration runtime state
    bool BackgroundCalibrationModule::resetState()
    {
        resetCalibration();
        return true;
    }

    // Clears the buffered frames and computed background
    void BackgroundCalibrationModule::resetCalibration()
    {
        m_buffer.clear();
        m_background = ImageFrame{};
        m_calibrated = false;
    }

    // Computes the background image from buffered calibration frames
    void BackgroundCalibrationModule::computeBackground()
    {
        const int w = m_buffer.front().width;
        const int h = m_buffer.front().height;
        const int maxValue = m_buffer.front().maxValue();
        QByteArray bgBytes = dispatchFrameType(m_buffer.front(), [&]<typename Pixel>()
        {
            QByteArray outBytes = allocatePixelBytes<Pixel>(w, h);
            if (outBytes.isEmpty())
            {
                return outBytes;
            }
            switch (m_method)
            {
            case BackgroundMethod::Median:
                {
                    std::vector<Pixel> vals(m_buffer.size());
                    for (int y = 0; y < h; ++y)
                    {
                        Pixel* dst = mutableRowData<Pixel>(outBytes, w, y);
                        for (int x = 0; x < w; ++x)
                        {
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                vals[k] = frameRowData<Pixel>(m_buffer[k], y)[x];
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
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                sum += static_cast<int>(frameRowData<Pixel>(m_buffer[k], y)[x]);
                            }
                            dst[x] = clampPixelValue<Pixel>(
                                static_cast<int>(sum / static_cast<qint64>(m_buffer.size())),
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
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                best = qMax(best, static_cast<int>(frameRowData<Pixel>(m_buffer[k], y)[x]));
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
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                best = qMin(best, static_cast<int>(frameRowData<Pixel>(m_buffer[k], y)[x]));
                            }
                            dst[x] = clampPixelValue<Pixel>(best, maxValue);
                        }
                    }
                    break;
                }
            }
            return outBytes;
        });

        m_background = makeFrameLike(m_buffer.front(), w, h, std::move(bgBytes));
    }

    // Applies background calibration to one mono frame
    ProcessingResult BackgroundCalibrationModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return {{}, QStringLiteral("Invalid input")};
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth))
            {
                return {{}, QStringLiteral("Unsupported input frame")};
            }

            if ((!m_buffer.empty() && !m_buffer.front().isCompatibleWith(workingFrame))
                || (m_background.isValid() && !m_background.isCompatibleWith(workingFrame)))
            {
                resetCalibration();
            }

            if (m_mode == BackgroundMode::Running)
            {
                if ((int)m_buffer.size() == m_calibrationFrames)
                {
                    computeBackground();
                }
                m_buffer.push_back(workingFrame);
                if ((int)m_buffer.size() > m_calibrationFrames)
                {
                    m_buffer.pop_front();
                }
                if (!m_background.isValid())
                {
                    return {frame, {}};
                }
            }
            else if (!m_calibrated)
            {
                m_buffer.push_back(workingFrame);
                if ((int)m_buffer.size() > m_calibrationFrames)
                {
                    m_buffer.pop_front();
                }
                if ((int)m_buffer.size() < m_calibrationFrames)
                {
                    return {frame, {}};
                }

                computeBackground();
                m_buffer.clear();
                m_calibrated = true;
            }

            if (m_background.isValid() && m_background.isCompatibleWith(workingFrame))
            {
                const int maxValue = workingFrame.maxValue();
                QByteArray outBytes = dispatchFrameType(workingFrame, [&]<typename Pixel>()
                {
                    QByteArray bytes = allocatePixelBytes<Pixel>(workingFrame.width, workingFrame.height);
                    if (bytes.isEmpty())
                    {
                        return bytes;
                    }
                    for (int y = 0; y < workingFrame.height; ++y)
                    {
                        const Pixel* s = frameRowData<Pixel>(workingFrame, y);
                        const Pixel* b = frameRowData<Pixel>(m_background, y);
                        Pixel* d = mutableRowData<Pixel>(bytes, workingFrame.width, y);
                        for (int x = 0; x < workingFrame.width; ++x)
                        {
                            int result = 0;
                            switch (m_operation)
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

                return {makeFrameLike(workingFrame,
                                      workingFrame.width,
                                      workingFrame.height,
                                      std::move(outBytes)),
                        {}};
            }
            return {workingFrame, {}};
        }
        catch (const std::exception& e)
        {
            return {{}, QString("Background calibration failed: %1").arg(e.what())};
        }
    }

    // Returns the current background calibration parameters
    QVariantMap BackgroundCalibrationModule::parameters() const
    {
        QVariantMap p;
        p["calibration_frames"] = m_calibrationFrames;
        p["operation"] = static_cast<int>(m_operation);
        p["method"] = static_cast<int>(m_method);
        p["mode"] = static_cast<int>(m_mode);
        return p;
    }

    // Updates calibration parameters and resets state when needed
    void BackgroundCalibrationModule::setParameters(const QVariantMap& params)
    {
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
            resetCalibration();
        }
    }
} // namespace scopeone::core::internal
