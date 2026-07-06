#include "internal/DifferentialRollingModule.h"

#include "internal/FrameBufferUtils.h"

namespace scopeone::core::internal
{
    namespace
    {
        constexpr double kNormalizedDisplayScale = 4096.0;
        constexpr double kNormalizationEpsilon = 1.0;

        qint64 pixelCountForSize(int width, int height)
        {
            if (width <= 0 || height <= 0)
            {
                return 0;
            }
            return static_cast<qint64>(width) * static_cast<qint64>(height);
        }

        // Adds or removes one frame from a rolling pixel sum
        void accumulateFrameSum(const ImageFrame& frame, std::vector<int>& sum, int sign)
        {
            dispatchFrameType(frame, [&]<typename Pixel>()
            {
                for (int y = 0; y < frame.height; ++y)
                {
                    const Pixel* row = frameRowData<Pixel>(frame, y);
                    const qint64 rowOffset = static_cast<qint64>(y) * static_cast<qint64>(frame.width);
                    for (int x = 0; x < frame.width; ++x)
                    {
                        sum[static_cast<size_t>(rowOffset + x)] += sign * static_cast<int>(row[x]);
                    }
                }
            });
        }

        // Adds one frame to a rolling pixel sum
        void addFrameToSum(const ImageFrame& frame, std::vector<int>& sum)
        {
            accumulateFrameSum(frame, sum, 1);
        }

        // Removes one frame from a rolling pixel sum
        void subtractFrameFromSum(const ImageFrame& frame, std::vector<int>& sum)
        {
            accumulateFrameSum(frame, sum, -1);
        }

        // Resets rolling state for a new frame size
        void resetState(DifferentialRollingModule::CameraState& state, const ImageFrame& frame)
        {
            state = DifferentialRollingModule::CameraState{};
            state.width = frame.width;
            state.height = frame.height;
            const qint64 pixelCount = pixelCountForSize(frame.width, frame.height);
            state.sumA.assign(static_cast<size_t>(pixelCount), 0);
            state.sumB.assign(static_cast<size_t>(pixelCount), 0);
        }

        // Builds the differential rolling output frame
        ImageFrame makeDifferentialOutput(const QString& cameraId,
                                          int width,
                                          int height,
                                          const ImageFrame& reference,
                                          const std::vector<int>& sumA,
                                          const std::vector<int>& sumB,
                                          int batchSize,
                                          bool normalize)
        {
            const int maxValue = reference.maxValue();
            const double centerValue = reference.isMono16() ? 32768.0 : 128.0;
            const double normalizationScale = reference.isMono16() ? 32767.0 : kNormalizedDisplayScale;
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
                for (qint64 i = 0; i < pixelCount; ++i)
                {
                    const double sum1 = static_cast<double>(sumA[static_cast<size_t>(i)]);
                    const double sum2 = static_cast<double>(sumB[static_cast<size_t>(i)]);
                    const double averageDiff = (sum2 - sum1) / static_cast<double>(batchSize);
                    double displayValue = averageDiff + centerValue;
                    if (normalize)
                    {
                        const double normalized = (sum2 - sum1)
                            / (sum1 > static_cast<double>(batchSize)
                                   ? sum1
                                   : static_cast<double>(batchSize) * kNormalizationEpsilon);
                        displayValue = centerValue + normalized * normalizationScale;
                    }
                    outData[i] = clampPixelValue<Pixel>(qRound(displayValue), maxValue);
                }
                return outBytes;
            });

            ImageFrame output = makeFrameLike(reference, width, height, std::move(bytes));
            output.cameraId = cameraId;
            return output;
        }
    } // namespace

    // Creates a differential rolling processing module
    DifferentialRollingModule::DifferentialRollingModule(QObject* parent)
        : ProcessingModule(parent)
    {
    }

    // Updates rolling batches and emits the current differential frame
    bool DifferentialRollingModule::process(const ModuleInput& in, ModuleOutput& out)
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

            const bool incompatibleBuffers = (!m_state.batchA.empty() && !m_state.batchA.front().isCompatibleWith(
                    workingFrame))
                || (!m_state.batchB.empty() && !m_state.batchB.front().isCompatibleWith(workingFrame));
            if (incompatibleBuffers
                || m_state.width != workingFrame.width
                || m_state.height != workingFrame.height
                || m_state.sumA.size() != static_cast<size_t>(pixelCountForSize(workingFrame.width, workingFrame.height))
                || m_state.sumB.size() != static_cast<size_t>(pixelCountForSize(workingFrame.width, workingFrame.height)))
            {
                resetState(m_state, workingFrame);
            }

            if (m_state.batchA.size() < static_cast<size_t>(m_batchSize))
            {
                m_state.batchA.push_back(workingFrame);
                addFrameToSum(workingFrame, m_state.sumA);
                out.frame = in.frame;
                return true;
            }

            if (m_state.batchB.size() < static_cast<size_t>(m_batchSize))
            {
                m_state.batchB.push_back(workingFrame);
                addFrameToSum(workingFrame, m_state.sumB);
                if (m_state.batchB.size() < static_cast<size_t>(m_batchSize))
                {
                    out.frame = in.frame;
                    return true;
                }

                out.frame = makeDifferentialOutput(in.frame.cameraId,
                                                   workingFrame.width,
                                                   workingFrame.height,
                                                   workingFrame,
                                                   m_state.sumA,
                                                   m_state.sumB,
                                                   m_batchSize,
                                                   m_normalize);
                return true;
            }

            const ImageFrame oldestA = m_state.batchA.front();
            const ImageFrame bridgeFrame = m_state.batchB.front();

            subtractFrameFromSum(oldestA, m_state.sumA);
            m_state.batchA.pop_front();
            m_state.batchA.push_back(bridgeFrame);
            addFrameToSum(bridgeFrame, m_state.sumA);

            subtractFrameFromSum(bridgeFrame, m_state.sumB);
            m_state.batchB.pop_front();
            m_state.batchB.push_back(workingFrame);
            addFrameToSum(workingFrame, m_state.sumB);

            out.frame = makeDifferentialOutput(in.frame.cameraId,
                                               workingFrame.width,
                                               workingFrame.height,
                                               workingFrame,
                                               m_state.sumA,
                                               m_state.sumB,
                                               m_batchSize,
                                               m_normalize);
            return true;
        }
        catch (const std::exception& e)
        {
            out.frame = in.frame;
            out.error = QString("Differential rolling failed: %1").arg(e.what());
            return false;
        }
    }

    // Returns the current rolling differential parameters
    QVariantMap DifferentialRollingModule::getParameters() const
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

    // Clears all accumulated rolling state
    void DifferentialRollingModule::resetBuffer()
    {
        m_state = CameraState{};
    }
} // namespace scopeone::core::internal
