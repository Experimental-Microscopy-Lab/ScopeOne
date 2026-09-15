#include "internal/DifferentialRollingModule.h"

#include "internal/FrameBufferUtils.h"

#include <limits>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr qint64 kNormalizedDisplayScale = 4096;

        qint64 pixelCountForSize(int width, int height)
        {
            if (width <= 0 || height <= 0)
            {
                return 0;
            }
            return static_cast<qint64>(width) * static_cast<qint64>(height);
        }

        // Adds one frame to a rolling pixel sum
        void addFrameToSum(const ImageFrame& frame, std::vector<int>& sum)
        {
            dispatchFrameType(frame, [&]<typename Pixel>()
            {
                const char* sourceData = frame.bytes.constData();
                int* sumData = sum.data();
                parallelForImageRows(frame.width, frame.height, [&](int firstRow, int lastRow)
                {
                    for (int y = firstRow; y < lastRow; ++y)
                    {
                        const Pixel* row = reinterpret_cast<const Pixel*>(
                            sourceData + static_cast<qint64>(y) * frame.stride);
                        int* sumRow = sumData + static_cast<size_t>(y) * frame.width;
                        for (int x = 0; x < frame.width; ++x)
                        {
                            sumRow[x] += static_cast<int>(row[x]);
                        }
                    }
                });
            });
        }

        qint64 roundedDivide(qint64 numerator, qint64 denominator)
        {
            return numerator >= 0
                       ? (numerator + denominator / 2) / denominator
                       : (numerator - denominator / 2) / denominator;
        }

        // Maps rolling sums to one display pixel
        int differentialDisplayValue(int sumA,
                                     int sumB,
                                     int batchSize,
                                     bool normalize,
                                     bool mono16)
        {
            const qint64 centerValue = mono16 ? 32768 : 128;
            const qint64 difference = static_cast<qint64>(sumB) - sumA;
            qint64 displayValue = centerValue + roundedDivide(difference, batchSize);
            if (normalize)
            {
                const qint64 normalizationScale = mono16 ? 32767 : kNormalizedDisplayScale;
                const qint64 denominator = sumA > batchSize ? sumA : batchSize;
                displayValue = centerValue
                    + roundedDivide(difference * normalizationScale, denominator);
            }
            return static_cast<int>(qBound(
                static_cast<qint64>((std::numeric_limits<int>::min)()),
                displayValue,
                static_cast<qint64>((std::numeric_limits<int>::max)())));
        }

        // Resets rolling state for a new frame size
        void initializeCameraState(DifferentialRollingModule::CameraState& state, const ImageFrame& frame)
        {
            state = DifferentialRollingModule::CameraState{};
            state.width = frame.width;
            state.height = frame.height;
            const qint64 pixelCount = pixelCountForSize(frame.width, frame.height);
            state.sumA.assign(static_cast<size_t>(pixelCount), 0);
            state.sumB.assign(static_cast<size_t>(pixelCount), 0);
        }

        // Builds the differential rolling output frame
        ImageFrame makeDifferentialOutput(int width,
                                          int height,
                                          const ImageFrame& reference,
                                          const std::vector<int>& sumA,
                                          const std::vector<int>& sumB,
                                          int batchSize,
                                          bool normalize)
        {
            const int maxValue = reference.maxValue();
            const bool mono16 = reference.isMono16();
            const qint64 pixelCount = pixelCountForSize(width, height);
            if (pixelCount <= 0)
            {
                return {};
            }
            QByteArray bytes = dispatchFrameType(reference, [&]<typename Pixel>()
            {
                QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
                if (outBytes.isEmpty())
                {
                    return outBytes;
                }
                auto* outData = reinterpret_cast<Pixel*>(outBytes.data());
                parallelForImageRows(width, height, [&](int firstRow, int lastRow)
                {
                    const qint64 firstPixel = static_cast<qint64>(firstRow) * width;
                    const qint64 lastPixel = static_cast<qint64>(lastRow) * width;
                    for (qint64 i = firstPixel; i < lastPixel; ++i)
                    {
                        outData[i] = clampPixelValue<Pixel>(
                            differentialDisplayValue(sumA[static_cast<size_t>(i)],
                                                     sumB[static_cast<size_t>(i)],
                                                     batchSize,
                                                     normalize,
                                                     mono16),
                            maxValue);
                    }
                });
                return outBytes;
            });

            return makeFrameLike(reference, width, height, std::move(bytes));
        }

        // Advances both rolling sums and builds the output in one image pass
        ImageFrame advanceRollingAndBuildOutput(const ImageFrame& oldestA,
                                                const ImageFrame& bridge,
                                                const ImageFrame& current,
                                                std::vector<int>& sumA,
                                                std::vector<int>& sumB,
                                                int batchSize,
                                                bool normalize)
        {
            const qint64 pixelCount = pixelCountForSize(current.width, current.height);
            if (pixelCount <= 0)
            {
                return {};
            }

            const int maxValue = current.maxValue();
            const bool mono16 = current.isMono16();
            QByteArray bytes = dispatchFrameType(current, [&]<typename Pixel>()
            {
                QByteArray outBytes = allocatePixelBytes<Pixel>(current.width, current.height);
                if (outBytes.isEmpty())
                {
                    return outBytes;
                }

                const char* oldestData = oldestA.bytes.constData();
                const char* bridgeData = bridge.bytes.constData();
                const char* currentData = current.bytes.constData();
                Pixel* outputData = reinterpret_cast<Pixel*>(outBytes.data());
                int* sumAData = sumA.data();
                int* sumBData = sumB.data();
                parallelForImageRows(current.width, current.height, [&](int firstRow, int lastRow)
                {
                    for (int y = firstRow; y < lastRow; ++y)
                    {
                        const Pixel* oldestRow = reinterpret_cast<const Pixel*>(
                            oldestData + static_cast<qint64>(y) * oldestA.stride);
                        const Pixel* bridgeRow = reinterpret_cast<const Pixel*>(
                            bridgeData + static_cast<qint64>(y) * bridge.stride);
                        const Pixel* currentRow = reinterpret_cast<const Pixel*>(
                            currentData + static_cast<qint64>(y) * current.stride);
                        const size_t rowOffset = static_cast<size_t>(y) * current.width;
                        Pixel* outputRow = outputData + rowOffset;
                        int* sumARow = sumAData + rowOffset;
                        int* sumBRow = sumBData + rowOffset;
                        for (int x = 0; x < current.width; ++x)
                        {
                            sumARow[x] += static_cast<int>(bridgeRow[x])
                                          - static_cast<int>(oldestRow[x]);
                            sumBRow[x] += static_cast<int>(currentRow[x])
                                          - static_cast<int>(bridgeRow[x]);
                            outputRow[x] = clampPixelValue<Pixel>(
                                differentialDisplayValue(sumARow[x],
                                                         sumBRow[x],
                                                         batchSize,
                                                         normalize,
                                                         mono16),
                                maxValue);
                        }
                    }
                });
                return outBytes;
            });

            return makeFrameLike(current,
                                 current.width,
                                 current.height,
                                 std::move(bytes));
        }
    } // namespace

    // Creates an independent differential rolling runtime
    std::unique_ptr<ProcessingModule> DifferentialRollingModule::createRuntime() const
    {
        auto module = std::make_unique<DifferentialRollingModule>();
        module->setParameters(parameters());
        return module;
    }

    // Updates rolling batches and emits the current differential frame
    ProcessingResult DifferentialRollingModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid input"));
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth))
            {
                return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported input frame"));
            }

            const bool incompatibleBuffers = (!m_state.batchA.empty() && !m_state.batchA.front().isCompatibleWith(
                    workingFrame))
                || (!m_state.batchB.empty() && !m_state.batchB.front().isCompatibleWith(workingFrame));
            if (incompatibleBuffers
                || m_state.width != workingFrame.width
                || m_state.height != workingFrame.height
                || m_state.sumA.size() != static_cast<size_t>(pixelCountForSize(workingFrame.width, workingFrame.height))
                || m_state.sumB.size() != static_cast<size_t>(pixelCountForSize(workingFrame.width, workingFrame.height)))
            {
                initializeCameraState(m_state, workingFrame);
            }

            if (m_state.batchA.size() < static_cast<size_t>(m_batchSize))
            {
                m_state.batchA.push_back(workingFrame);
                addFrameToSum(workingFrame, m_state.sumA);
                return {frame, {}};
            }

            if (m_state.batchB.size() < static_cast<size_t>(m_batchSize))
            {
                m_state.batchB.push_back(workingFrame);
                addFrameToSum(workingFrame, m_state.sumB);
                if (m_state.batchB.size() < static_cast<size_t>(m_batchSize))
                {
                    return {frame, {}};
                }

                return {makeDifferentialOutput(workingFrame.width,
                                               workingFrame.height,
                                               workingFrame,
                                               m_state.sumA,
                                               m_state.sumB,
                                               m_batchSize,
                                               m_normalize),
                        {}};
            }

            const ImageFrame oldestA = m_state.batchA.front();
            const ImageFrame bridgeFrame = m_state.batchB.front();
            ImageFrame output = advanceRollingAndBuildOutput(oldestA,
                                                             bridgeFrame,
                                                             workingFrame,
                                                             m_state.sumA,
                                                             m_state.sumB,
                                                             m_batchSize,
                                                             m_normalize);
            m_state.batchA.pop_front();
            m_state.batchA.push_back(bridgeFrame);
            m_state.batchB.pop_front();
            m_state.batchB.push_back(workingFrame);

            return {std::move(output), {}};
        }
        catch (const std::exception& e)
        {
            return ProcessingResult(ImageFrame{}, QString("Differential rolling failed: %1").arg(e.what()));
        }
    }

    // Returns the current rolling differential parameters
    QVariantMap DifferentialRollingModule::parameters() const
    {
        QVariantMap params;
        params["batch_size"] = m_batchSize;
        params["normalize"] = m_normalize;
        return params;
    }

    // Updates rolling differential parameters
    void DifferentialRollingModule::setParameters(const QVariantMap& params)
    {
        bool resetState = false;

        if (params.contains("batch_size"))
        {
            const int batchSize = qMax(1, params.value("batch_size").toInt());
            if (batchSize != m_batchSize)
            {
                m_batchSize = batchSize;
                resetState = true;
            }
        }
        if (params.contains("normalize"))
        {
            m_normalize = params.value("normalize").toBool();
        }

        if (resetState)
        {
            m_state = CameraState{};
        }
    }

    // Clears differential rolling runtime state
    bool DifferentialRollingModule::resetState()
    {
        m_state = CameraState{};
        return true;
    }
} // namespace scopeone::core::internal
