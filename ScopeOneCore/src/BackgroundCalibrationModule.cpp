#include "internal/BackgroundCalibrationModule.h"
#include "internal/FrameBufferUtils.h"
#include <algorithm>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        // Applies one calibrated background sample to one source sample
        int applyBackgroundOperation(int source,
                                     int background,
                                     int maxValue,
                                     BackgroundOperation operation)
        {
            switch (operation)
            {
            case BackgroundOperation::Subtract:
                return source - background;
            case BackgroundOperation::Add:
                return source + background;
            case BackgroundOperation::Multiply:
                return static_cast<int>((static_cast<qint64>(source) * background) / maxValue);
            case BackgroundOperation::Divide:
                return background > 0
                           ? static_cast<int>((static_cast<qint64>(source) * maxValue) / background)
                           : maxValue;
            }
            return source;
        }

        // Adds one frame to a full precision running pixel sum
        void addFrameToRunningSum(const ImageFrame& frame, std::vector<qint64>& sum)
        {
            dispatchFrameType(frame, [&]<typename Pixel>()
            {
                const char* sourceData = frame.bytes.constData();
                qint64* sumData = sum.data();
                parallelForImageRows(frame.width, frame.height, [&](int firstRow, int lastRow)
                {
                    for (int y = firstRow; y < lastRow; ++y)
                    {
                        const Pixel* row = reinterpret_cast<const Pixel*>(
                            sourceData + static_cast<qint64>(y) * frame.stride);
                        qint64* sumRow = sumData + static_cast<size_t>(y) * frame.width;
                        for (int x = 0; x < frame.width; ++x)
                        {
                            sumRow[x] += static_cast<int>(row[x]);
                        }
                    }
                });
            });
        }
    } // namespace

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
        m_runningSum.clear();
        m_background = ImageFrame{};
        m_calibrated = false;
    }

    // Computes the background image from buffered calibration frames
    void BackgroundCalibrationModule::computeBackground()
    {
        const int w = m_buffer.front().width;
        const int h = m_buffer.front().height;
        const int maxValue = m_buffer.front().maxValue();
        const qint64 workItemCount = static_cast<qint64>(w)
                                       * h * static_cast<qint64>(m_buffer.size());
        QByteArray bgBytes = dispatchFrameType(m_buffer.front(), [&]<typename Pixel>()
        {
            QByteArray outBytes = allocatePixelBytes<Pixel>(w, h);
            if (outBytes.isEmpty())
            {
                return outBytes;
            }
            Pixel* outputData = reinterpret_cast<Pixel*>(outBytes.data());
            switch (m_method)
            {
            case BackgroundMethod::Median:
                {
                    parallelForRows(workItemCount, h, [&](int firstRow, int lastRow)
                    {
                        std::vector<Pixel> vals(m_buffer.size());
                        std::vector<const Pixel*> rows(m_buffer.size());
                        for (int y = firstRow; y < lastRow; ++y)
                        {
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                rows[k] = frameRowData<Pixel>(m_buffer[k], y);
                            }
                            Pixel* dst = outputData + static_cast<size_t>(y) * w;
                            for (int x = 0; x < w; ++x)
                            {
                                for (size_t k = 0; k < m_buffer.size(); ++k)
                                {
                                    vals[k] = rows[k][x];
                                }
                                std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                                dst[x] = vals[vals.size() / 2];
                            }
                        }
                    });
                    break;
                }
            case BackgroundMethod::Mean:
                {
                    parallelForRows(workItemCount, h, [&](int firstRow, int lastRow)
                    {
                        std::vector<const Pixel*> rows(m_buffer.size());
                        for (int y = firstRow; y < lastRow; ++y)
                        {
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                rows[k] = frameRowData<Pixel>(m_buffer[k], y);
                            }
                            Pixel* dst = outputData + static_cast<size_t>(y) * w;
                            for (int x = 0; x < w; ++x)
                            {
                                qint64 sum = 0;
                                for (size_t k = 0; k < m_buffer.size(); ++k)
                                {
                                    sum += static_cast<int>(rows[k][x]);
                                }
                                dst[x] = clampPixelValue<Pixel>(
                                    static_cast<int>(sum / static_cast<qint64>(m_buffer.size())),
                                    maxValue);
                            }
                        }
                    });
                    break;
                }
            case BackgroundMethod::Maximum:
                {
                    parallelForRows(workItemCount, h, [&](int firstRow, int lastRow)
                    {
                        std::vector<const Pixel*> rows(m_buffer.size());
                        for (int y = firstRow; y < lastRow; ++y)
                        {
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                rows[k] = frameRowData<Pixel>(m_buffer[k], y);
                            }
                            Pixel* dst = outputData + static_cast<size_t>(y) * w;
                            for (int x = 0; x < w; ++x)
                            {
                                int best = 0;
                                for (size_t k = 0; k < m_buffer.size(); ++k)
                                {
                                    best = qMax(best, static_cast<int>(rows[k][x]));
                                }
                                dst[x] = clampPixelValue<Pixel>(best, maxValue);
                            }
                        }
                    });
                    break;
                }
            case BackgroundMethod::Minimum:
                {
                    parallelForRows(workItemCount, h, [&](int firstRow, int lastRow)
                    {
                        std::vector<const Pixel*> rows(m_buffer.size());
                        for (int y = firstRow; y < lastRow; ++y)
                        {
                            for (size_t k = 0; k < m_buffer.size(); ++k)
                            {
                                rows[k] = frameRowData<Pixel>(m_buffer[k], y);
                            }
                            Pixel* dst = outputData + static_cast<size_t>(y) * w;
                            for (int x = 0; x < w; ++x)
                            {
                                int best = maxValue;
                                for (size_t k = 0; k < m_buffer.size(); ++k)
                                {
                                    best = qMin(best, static_cast<int>(rows[k][x]));
                                }
                                dst[x] = clampPixelValue<Pixel>(best, maxValue);
                            }
                        }
                    });
                    break;
                }
            }
            return outBytes;
        });

        m_background = makeFrameLike(m_buffer.front(), w, h, std::move(bgBytes));
    }

    // Updates a running mean background and applies it in one image pass
    ProcessingResult BackgroundCalibrationModule::processRunningMean(
        const ImageFrame& sourceFrame,
        const ImageFrame& workingFrame)
    {
        const qint64 pixelCount = static_cast<qint64>(workingFrame.width) * workingFrame.height;
        if (pixelCount <= 0)
        {
            return {{}, QStringLiteral("Invalid running background dimensions")};
        }
        if (m_runningSum.size() != static_cast<size_t>(pixelCount))
        {
            m_buffer.clear();
            m_runningSum.assign(static_cast<size_t>(pixelCount), 0);
            m_background = ImageFrame{};
        }

        if (static_cast<int>(m_buffer.size()) < m_calibrationFrames)
        {
            addFrameToRunningSum(workingFrame, m_runningSum);
            m_buffer.push_back(workingFrame);
            return {sourceFrame, {}};
        }

        const ImageFrame& oldestFrame = m_buffer.front();
        const int maxValue = workingFrame.maxValue();
        QByteArray outputBytes;
        dispatchFrameType(workingFrame, [&]<typename Pixel>()
        {
            outputBytes = allocatePixelBytes<Pixel>(workingFrame.width, workingFrame.height);
            if (outputBytes.isEmpty())
            {
                return;
            }

            const char* sourceData = workingFrame.bytes.constData();
            const char* oldestData = oldestFrame.bytes.constData();
            Pixel* outputData = reinterpret_cast<Pixel*>(outputBytes.data());
            qint64* sumData = m_runningSum.data();
            parallelForImageRows(workingFrame.width,
                                 workingFrame.height,
                                 [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    const Pixel* sourceRow = reinterpret_cast<const Pixel*>(
                        sourceData + static_cast<qint64>(y) * workingFrame.stride);
                    const Pixel* oldestRow = reinterpret_cast<const Pixel*>(
                        oldestData + static_cast<qint64>(y) * oldestFrame.stride);
                    const size_t rowOffset = static_cast<size_t>(y) * workingFrame.width;
                    Pixel* outputRow = outputData + rowOffset;
                    qint64* sumRow = sumData + rowOffset;
                    for (int x = 0; x < workingFrame.width; ++x)
                    {
                        const int source = static_cast<int>(sourceRow[x]);
                        const int background = static_cast<int>(
                            sumRow[x] / static_cast<qint64>(m_calibrationFrames));
                        outputRow[x] = clampPixelValue<Pixel>(
                            applyBackgroundOperation(source,
                                                     background,
                                                     maxValue,
                                                     m_operation),
                            maxValue);
                        sumRow[x] += source - static_cast<int>(oldestRow[x]);
                    }
                }
            });
        });

        if (outputBytes.isEmpty())
        {
            return {{}, QStringLiteral("Failed to allocate running background output")};
        }

        ImageFrame output = makeFrameLike(workingFrame,
                                          workingFrame.width,
                                          workingFrame.height,
                                          std::move(outputBytes));
        m_buffer.pop_front();
        m_buffer.push_back(workingFrame);
        return {std::move(output), {}};
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

            if (m_mode == BackgroundMode::Running && m_method == BackgroundMethod::Mean)
            {
                return processRunningMean(frame, workingFrame);
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
                    const char* sourceData = workingFrame.bytes.constData();
                    const char* backgroundData = m_background.bytes.constData();
                    Pixel* outputData = reinterpret_cast<Pixel*>(bytes.data());
                    parallelForImageRows(workingFrame.width,
                                         workingFrame.height,
                                         [&](int firstRow, int lastRow)
                    {
                        for (int y = firstRow; y < lastRow; ++y)
                        {
                            const Pixel* s = reinterpret_cast<const Pixel*>(
                                sourceData + static_cast<qint64>(y) * workingFrame.stride);
                            const Pixel* b = reinterpret_cast<const Pixel*>(
                                backgroundData + static_cast<qint64>(y) * m_background.stride);
                            Pixel* d = outputData + static_cast<size_t>(y) * workingFrame.width;
                            for (int x = 0; x < workingFrame.width; ++x)
                            {
                                d[x] = clampPixelValue<Pixel>(
                                    applyBackgroundOperation(static_cast<int>(s[x]),
                                                             static_cast<int>(b[x]),
                                                             maxValue,
                                                             m_operation),
                                    maxValue);
                            }
                        }
                    });
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
