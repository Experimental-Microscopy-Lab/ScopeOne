#include "internal/SpatiotemporalBinningModule.h"
#include "internal/FrameBufferUtils.h"

#include <QtGlobal>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        // Check whether a serialized binning mode maps to a supported enum value
        bool isValidBinningMode(int mode)
        {
            return mode >= static_cast<int>(SpatiotemporalBinningModule::BinningMode::Mean)
                && mode <= static_cast<int>(SpatiotemporalBinningModule::BinningMode::Skip);
        }

        template <SpatiotemporalBinningModule::BinningMode Mode, typename Pixel>
        int reduceTemporalExtremaPixel(const std::vector<const Pixel*>& rows,
                                       int x,
                                       int maxValue)
        {
            static_assert(Mode == SpatiotemporalBinningModule::BinningMode::Minimum
                          || Mode == SpatiotemporalBinningModule::BinningMode::Maximum);

            if constexpr (Mode == SpatiotemporalBinningModule::BinningMode::Minimum)
            {
                int value = maxValue;
                for (const Pixel* row : rows)
                {
                    value = qMin(value, static_cast<int>(row[x]));
                }
                return value;
            }
            else
            {
                int value = 0;
                for (const Pixel* row : rows)
                {
                    value = qMax(value, static_cast<int>(row[x]));
                }
                return value;
            }
        }

        template <SpatiotemporalBinningModule::BinningMode Mode, typename Pixel>
        QByteArray temporalExtremaBytes(const std::deque<ImageFrame>& buffer,
                                        int width,
                                        int height,
                                        int maxValue)
        {
            QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
            if (outBytes.isEmpty())
            {
                return outBytes;
            }

            Pixel* outputData = reinterpret_cast<Pixel*>(outBytes.data());
            std::vector<const char*> sourceData(buffer.size());
            std::vector<int> sourceStrides(buffer.size());
            for (size_t i = 0; i < buffer.size(); ++i)
            {
                sourceData[i] = buffer[i].bytes.constData();
                sourceStrides[i] = buffer[i].stride;
            }
            const qint64 workItemCount = static_cast<qint64>(width)
                                         * height * static_cast<qint64>(buffer.size());
            parallelForRows(workItemCount, height, [&](int firstRow, int lastRow)
            {
                std::vector<const Pixel*> rows(buffer.size());
                for (int y = firstRow; y < lastRow; ++y)
                {
                    for (size_t i = 0; i < buffer.size(); ++i)
                    {
                        rows[i] = reinterpret_cast<const Pixel*>(
                            sourceData[i] + static_cast<qint64>(y) * sourceStrides[i]);
                    }
                    Pixel* dstRow = outputData + static_cast<size_t>(y) * width;
                    for (int x = 0; x < width; ++x)
                    {
                        dstRow[x] = static_cast<Pixel>(
                            reduceTemporalExtremaPixel<Mode>(rows, x, maxValue));
                    }
                }
            });
            return outBytes;
        }

        // Advances a rolling temporal sum and writes a ready output in the same image pass
        ImageFrame advanceTemporalSum(const ImageFrame& incoming,
                                      const ImageFrame* expired,
                                      const ImageFrame& reference,
                                      std::vector<qint64>& sum,
                                      int frameCount,
                                      bool mean,
                                      bool outputReady)
        {
            const int maxValue = incoming.maxValue();
            QByteArray bytes = dispatchFrameType(incoming, [&]<typename Pixel>()
            {
                QByteArray outBytes;
                if (outputReady)
                {
                    outBytes = allocatePixelBytes<Pixel>(incoming.width, incoming.height);
                    if (outBytes.isEmpty())
                    {
                        return outBytes;
                    }
                }

                const char* incomingData = incoming.bytes.constData();
                const char* expiredData = expired ? expired->bytes.constData() : nullptr;
                qint64* sumData = sum.data();
                Pixel* outputData = outputReady
                                        ? reinterpret_cast<Pixel*>(outBytes.data())
                                        : nullptr;
                parallelForImageRows(incoming.width, incoming.height, [&](int firstRow, int lastRow)
                {
                    for (int y = firstRow; y < lastRow; ++y)
                    {
                        const Pixel* incomingRow = reinterpret_cast<const Pixel*>(
                            incomingData + static_cast<qint64>(y) * incoming.stride);
                        const Pixel* expiredRow = expired
                                                      ? reinterpret_cast<const Pixel*>(
                                                            expiredData
                                                            + static_cast<qint64>(y) * expired->stride)
                                                      : nullptr;
                        qint64* sumRow = sumData + static_cast<size_t>(y) * incoming.width;
                        Pixel* outputRow = outputData
                                               ? outputData + static_cast<size_t>(y) * incoming.width
                                               : nullptr;
                        for (int x = 0; x < incoming.width; ++x)
                        {
                            sumRow[x] += static_cast<int>(incomingRow[x]);
                            if (expiredRow)
                            {
                                sumRow[x] -= static_cast<int>(expiredRow[x]);
                            }
                            if (outputRow)
                            {
                                const qint64 value = mean
                                                         ? sumRow[x] / frameCount
                                                         : sumRow[x];
                                outputRow[x] = static_cast<Pixel>(
                                    qBound(qint64{0}, value, static_cast<qint64>(maxValue)));
                            }
                        }
                    }
                });
                return outBytes;
            });
            if (!outputReady)
            {
                return {};
            }
            return makeFrameLike(reference,
                                 incoming.width,
                                 incoming.height,
                                 std::move(bytes));
        }

        template <SpatiotemporalBinningModule::BinningMode Mode, typename Pixel>
        int reduceSpatialBlock(const char* sourceData,
                               int sourceStride,
                               int startX,
                               int startY,
                               int binX,
                               int binY,
                               int maxValue)
        {
            if constexpr (Mode == SpatiotemporalBinningModule::BinningMode::Skip)
            {
                const Pixel* row = reinterpret_cast<const Pixel*>(
                    sourceData + static_cast<qint64>(startY) * sourceStride);
                return static_cast<int>(row[startX]);
            }

            if constexpr (Mode == SpatiotemporalBinningModule::BinningMode::Minimum)
            {
                int value = maxValue;
                for (int yy = 0; yy < binY; ++yy)
                {
                    const Pixel* row = reinterpret_cast<const Pixel*>(
                        sourceData + static_cast<qint64>(startY + yy) * sourceStride) + startX;
                    for (int xx = 0; xx < binX; ++xx)
                    {
                        value = qMin(value, static_cast<int>(row[xx]));
                    }
                }
                return value;
            }

            if constexpr (Mode == SpatiotemporalBinningModule::BinningMode::Maximum)
            {
                int value = 0;
                for (int yy = 0; yy < binY; ++yy)
                {
                    const Pixel* row = reinterpret_cast<const Pixel*>(
                        sourceData + static_cast<qint64>(startY + yy) * sourceStride) + startX;
                    for (int xx = 0; xx < binX; ++xx)
                    {
                        value = qMax(value, static_cast<int>(row[xx]));
                    }
                }
                return value;
            }

            qint64 sum = 0;
            for (int yy = 0; yy < binY; ++yy)
            {
                const Pixel* row = reinterpret_cast<const Pixel*>(
                    sourceData + static_cast<qint64>(startY + yy) * sourceStride) + startX;
                for (int xx = 0; xx < binX; ++xx)
                {
                    sum += static_cast<int>(row[xx]);
                }
            }
            if constexpr (Mode == SpatiotemporalBinningModule::BinningMode::Mean)
            {
                sum /= static_cast<qint64>(binX) * binY;
            }
            return static_cast<int>(qBound(qint64{0}, sum, static_cast<qint64>(maxValue)));
        }

        template <SpatiotemporalBinningModule::BinningMode Mode, typename Pixel>
        QByteArray spatialBinningBytes(const ImageFrame& frame,
                                       int width,
                                       int height,
                                       int binX,
                                       int binY,
                                       int maxValue)
        {
            QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
            if (outBytes.isEmpty())
            {
                return outBytes;
            }

            const char* sourceData = frame.bytes.constData();
            Pixel* outputData = reinterpret_cast<Pixel*>(outBytes.data());
            const qint64 workItemCount = static_cast<qint64>(width)
                                         * height * binX * binY;
            parallelForRows(workItemCount, height, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    Pixel* dst = outputData + static_cast<size_t>(y) * width;
                    for (int x = 0; x < width; ++x)
                    {
                        dst[x] = static_cast<Pixel>(
                            reduceSpatialBlock<Mode, Pixel>(sourceData,
                                                           frame.stride,
                                                           x * binX,
                                                           y * binY,
                                                           binX,
                                                           binY,
                                                           maxValue));
                    }
                }
            });
            return outBytes;
        }

        // Scans one buffered window for temporal minimum or maximum
        ImageFrame applyTemporalExtrema(const std::deque<ImageFrame>& buffer,
                                        SpatiotemporalBinningModule::BinningMode mode)
        {
            if (buffer.empty())
            {
                qFatal("Temporal binning requires at least one frame");
            }
            if (mode != SpatiotemporalBinningModule::BinningMode::Minimum
                && mode != SpatiotemporalBinningModule::BinningMode::Maximum)
            {
                qFatal("Temporal extrema requires minimum or maximum mode");
            }

            const int width = buffer.front().width;
            const int height = buffer.front().height;
            const int maxValue = buffer.front().maxValue();
            QByteArray bytes = dispatchFrameType(buffer.front(), [&]<typename Pixel>()
            {
                if (mode == SpatiotemporalBinningModule::BinningMode::Minimum)
                {
                    return temporalExtremaBytes<SpatiotemporalBinningModule::BinningMode::Minimum, Pixel>(
                        buffer, width, height, maxValue);
                }
                return temporalExtremaBytes<SpatiotemporalBinningModule::BinningMode::Maximum, Pixel>(
                    buffer, width, height, maxValue);
            });

            return makeFrameLike(buffer.front(), width, height, std::move(bytes));
        }

        // Combines neighboring pixels into spatial bins
        ImageFrame applySpatialBinning(const ImageFrame& frame,
                                       int binX,
                                       int binY,
                                       SpatiotemporalBinningModule::BinningMode mode)
        {
            if (!frame.isValid() || (binX <= 1 && binY <= 1))
            {
                return frame;
            }

            const int width = frame.width / qMax(1, binX);
            const int height = frame.height / qMax(1, binY);
            if (width <= 0 || height <= 0)
            {
                return frame;
            }

            const int maxValue = frame.maxValue();
            QByteArray bytes = dispatchFrameType(frame, [&]<typename Pixel>()
            {
                switch (mode)
                {
                case SpatiotemporalBinningModule::BinningMode::Mean:
                    return spatialBinningBytes<SpatiotemporalBinningModule::BinningMode::Mean, Pixel>(
                        frame, width, height, binX, binY, maxValue);
                case SpatiotemporalBinningModule::BinningMode::Sum:
                    return spatialBinningBytes<SpatiotemporalBinningModule::BinningMode::Sum, Pixel>(
                        frame, width, height, binX, binY, maxValue);
                case SpatiotemporalBinningModule::BinningMode::Minimum:
                    return spatialBinningBytes<SpatiotemporalBinningModule::BinningMode::Minimum, Pixel>(
                        frame, width, height, binX, binY, maxValue);
                case SpatiotemporalBinningModule::BinningMode::Maximum:
                    return spatialBinningBytes<SpatiotemporalBinningModule::BinningMode::Maximum, Pixel>(
                        frame, width, height, binX, binY, maxValue);
                case SpatiotemporalBinningModule::BinningMode::Skip:
                    return spatialBinningBytes<SpatiotemporalBinningModule::BinningMode::Skip, Pixel>(
                        frame, width, height, binX, binY, maxValue);
                }
                return QByteArray{};
            });

            return makeFrameLike(frame, width, height, std::move(bytes));
        }
    } // namespace

    // Creates an independent spatiotemporal binning runtime
    std::unique_ptr<ProcessingModule> SpatiotemporalBinningModule::createRuntime() const
    {
        auto module = std::make_unique<SpatiotemporalBinningModule>();
        module->setParameters(parameters());
        return module;
    }

    // Updates temporal history and emits a binned frame when ready
    ProcessingResult SpatiotemporalBinningModule::process(const ImageFrame& frame, int processingBitDepth)
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
            if (!m_frameBuffer.empty() && !m_frameBuffer.front().isCompatibleWith(workingFrame))
            {
                m_frameBuffer.clear();
                m_temporalSum.clear();
            }

            ImageFrame temporal;
            const bool useRollingSum = m_temporalBin > 1
                && (m_temporalMode == BinningMode::Mean
                    || m_temporalMode == BinningMode::Sum);
            if (useRollingSum)
            {
                const qint64 pixelCount = static_cast<qint64>(workingFrame.width)
                                          * workingFrame.height;
                if (m_temporalSum.size() != static_cast<size_t>(pixelCount))
                {
                    m_frameBuffer.clear();
                    m_temporalSum.assign(static_cast<size_t>(pixelCount), qint64{0});
                }
                const ImageFrame* expired = static_cast<int>(m_frameBuffer.size()) >= m_temporalBin
                                                ? &m_frameBuffer.front()
                                                : nullptr;
                const bool outputReady = static_cast<int>(m_frameBuffer.size()) + 1
                                         >= m_temporalBin;
                const ImageFrame& reference = m_frameBuffer.empty()
                                                  ? workingFrame
                                                  : m_frameBuffer.front();
                temporal = advanceTemporalSum(
                    workingFrame,
                    expired,
                    reference,
                    m_temporalSum,
                    m_temporalBin,
                    m_temporalMode == BinningMode::Mean,
                    outputReady);
            }
            m_frameBuffer.push_back(workingFrame);
            while (static_cast<int>(m_frameBuffer.size()) > m_temporalBin)
            {
                m_frameBuffer.pop_front();
            }

            if (static_cast<int>(m_frameBuffer.size()) < m_temporalBin)
            {
                return {frame, {}};
            }
            if (!useRollingSum)
            {
                temporal = m_frameBuffer.size() == 1 || m_temporalMode == BinningMode::Skip
                               ? m_frameBuffer.front()
                               : applyTemporalExtrema(m_frameBuffer, m_temporalMode);
            }
            return {applySpatialBinning(temporal, m_spatialBinX, m_spatialBinY, m_spatialMode), {}};
        }
        catch (const std::exception& e)
        {
            return {{}, QString("Spatiotemporal binning failed: %1").arg(e.what())};
        }
    }

    // Returns the current spatiotemporal binning parameters
    QVariantMap SpatiotemporalBinningModule::parameters() const
    {
        QVariantMap params;
        params["spatial_bin_x"] = m_spatialBinX;
        params["spatial_bin_y"] = m_spatialBinY;
        params["temporal_bin"] = m_temporalBin;
        params["spatial_mode"] = static_cast<int>(m_spatialMode);
        params["temporal_mode"] = static_cast<int>(m_temporalMode);
        return params;
    }

    // Updates spatiotemporal binning parameters
    void SpatiotemporalBinningModule::setParameters(const QVariantMap& params)
    {
        bool resetBuffer = false;

        if (params.contains("spatial_bin_x"))
        {
            m_spatialBinX = qMax(1, params.value("spatial_bin_x").toInt());
        }
        if (params.contains("spatial_bin_y"))
        {
            m_spatialBinY = qMax(1, params.value("spatial_bin_y").toInt());
        }
        if (params.contains("temporal_bin"))
        {
            const int temporalBin = qMax(1, params.value("temporal_bin").toInt());
            if (temporalBin != m_temporalBin)
            {
                m_temporalBin = temporalBin;
                resetBuffer = true;
            }
        }
        if (params.contains("spatial_mode"))
        {
            const int spatialMode = params.value("spatial_mode").toInt();
            if (isValidBinningMode(spatialMode))
            {
                m_spatialMode = static_cast<BinningMode>(spatialMode);
            }
        }
        if (params.contains("temporal_mode"))
        {
            const int temporalModeValue = params.value("temporal_mode").toInt();
            if (isValidBinningMode(temporalModeValue))
            {
                const BinningMode temporalMode = static_cast<BinningMode>(temporalModeValue);
                if (temporalMode != m_temporalMode)
                {
                    m_temporalMode = temporalMode;
                    resetBuffer = true;
                }
            }
        }

        if (resetBuffer)
        {
            m_frameBuffer.clear();
            m_temporalSum.clear();
        }
    }

    // Clears temporal binning runtime state
    bool SpatiotemporalBinningModule::resetState()
    {
        m_frameBuffer.clear();
        m_temporalSum.clear();
        return true;
    }
} // namespace scopeone::core::internal
