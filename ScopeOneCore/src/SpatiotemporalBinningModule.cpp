#include "internal/SpatiotemporalBinningModule.h"
#include "internal/FrameBufferUtils.h"

#include <QMutexLocker>

#include <algorithm>

namespace scopeone::core::internal {

namespace {

int modeValue(SpatiotemporalBinningModule::BinningMode mode, const std::vector<int>& values)
{
    if (values.empty()) {
        return 0;
    }

    switch (mode) {
    case SpatiotemporalBinningModule::BinningMode::Mean: {
        int sum = 0;
        for (int value : values) {
            sum += value;
        }
        return sum / static_cast<int>(values.size());
    }
    case SpatiotemporalBinningModule::BinningMode::Sum: {
        int sum = 0;
        for (int value : values) {
            sum += value;
        }
        return sum;
    }
    case SpatiotemporalBinningModule::BinningMode::Minimum:
        return *std::min_element(values.begin(), values.end());
    case SpatiotemporalBinningModule::BinningMode::Maximum:
        return *std::max_element(values.begin(), values.end());
    case SpatiotemporalBinningModule::BinningMode::Skip:
        return values.front();
    }
    return values.front();
}

ImageFrame applyTemporalBinning(const std::deque<ImageFrame>& buffer,
                                SpatiotemporalBinningModule::BinningMode mode)
{
    if (buffer.empty()) {
        return {};
    }
    if (buffer.size() == 1 || mode == SpatiotemporalBinningModule::BinningMode::Skip) {
        return buffer.front();
    }

    const int width = buffer.front().width;
    const int height = buffer.front().height;
    const int maxValue = buffer.front().maxValue();
    std::vector<int> samples(buffer.size());
    QByteArray bytes = dispatchFrameType(buffer.front(), [&]<typename Pixel>()
    {
        QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
        for (int y = 0; y < height; ++y) {
            Pixel* dstRow = mutableRowData<Pixel>(outBytes, width, y);
            for (int x = 0; x < width; ++x) {
                for (int i = 0; i < static_cast<int>(buffer.size()); ++i) {
                    samples[static_cast<size_t>(i)] = static_cast<int>(
                        frameRowData<Pixel>(buffer[static_cast<size_t>(i)], y)[x]);
                }
                dstRow[x] = clampPixelValue<Pixel>(modeValue(mode, samples), maxValue);
            }
        }
        return outBytes;
    });

    return makeFrameLike(buffer.front(), width, height, std::move(bytes));
}

ImageFrame applySpatialBinning(const ImageFrame& frame,
                               int binX,
                               int binY,
                               SpatiotemporalBinningModule::BinningMode mode)
{
    if (!frame.isValid() || (binX <= 1 && binY <= 1)) {
        return frame;
    }

    const int width = frame.width / qMax(1, binX);
    const int height = frame.height / qMax(1, binY);
    if (width <= 0 || height <= 0) {
        return frame;
    }

    std::vector<int> samples;
    samples.reserve(static_cast<size_t>(binX * binY));
    const int maxValue = frame.maxValue();
    QByteArray bytes = dispatchFrameType(frame, [&]<typename Pixel>()
    {
        QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
        for (int y = 0; y < height; ++y) {
            Pixel* dst = mutableRowData<Pixel>(outBytes, width, y);
            for (int x = 0; x < width; ++x) {
                samples.clear();
                for (int yy = 0; yy < binY; ++yy) {
                    for (int xx = 0; xx < binX; ++xx) {
                        samples.push_back(static_cast<int>(
                            frameRowData<Pixel>(frame, y * binY + yy)[x * binX + xx]));
                    }
                }
                dst[x] = clampPixelValue<Pixel>(modeValue(mode, samples), maxValue);
            }
        }
        return outBytes;
    });

    return makeFrameLike(frame, width, height, std::move(bytes));
}

QString frameHistoryKey(const ImageFrame& frame)
{
    return frame.cameraId.isEmpty() ? QStringLiteral("__default__") : frame.cameraId;
}

} // namespace

SpatiotemporalBinningModule::SpatiotemporalBinningModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool SpatiotemporalBinningModule::process(const ModuleInput& in, ModuleOutput& out)
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
        FrameBuffer frameBuffer;
        int spatialBinX = 1;
        int spatialBinY = 1;
        int temporalBin = 1;
        BinningMode spatialMode = BinningMode::Mean;
        BinningMode temporalMode = BinningMode::Mean;
        {
            QMutexLocker locker(&m_mutex);
            FrameBuffer& cameraBuffer = m_frameBuffers[frameHistoryKey(in.frame)];
            if (!cameraBuffer.empty() && !cameraBuffer.front().isCompatibleWith(workingFrame)) {
                cameraBuffer.clear();
            }
            cameraBuffer.push_back(workingFrame);
            while (static_cast<int>(cameraBuffer.size()) > m_temporalBin) {
                cameraBuffer.pop_front();
            }

            frameBuffer = cameraBuffer;
            spatialBinX = m_spatialBinX;
            spatialBinY = m_spatialBinY;
            temporalBin = m_temporalBin;
            spatialMode = m_spatialMode;
            temporalMode = m_temporalMode;
        }

        if (static_cast<int>(frameBuffer.size()) < temporalBin) {
            out.frame = in.frame;
        } else {
            const ImageFrame temporal = applyTemporalBinning(frameBuffer, temporalMode);
            out.frame = applySpatialBinning(temporal, spatialBinX, spatialBinY, spatialMode);
        }
    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Spatiotemporal binning failed: %1").arg(e.what());
        return false;
    }
    return true;
}

QVariantMap SpatiotemporalBinningModule::getParameters() const
{
    QVariantMap params;
    params["spatial_bin_x"] = m_spatialBinX;
    params["spatial_bin_y"] = m_spatialBinY;
    params["temporal_bin"] = m_temporalBin;
    params["spatial_mode"] = static_cast<int>(m_spatialMode);
    params["temporal_mode"] = static_cast<int>(m_temporalMode);
    return params;
}

void SpatiotemporalBinningModule::setParameters(const QVariantMap& params)
{
    QMutexLocker locker(&m_mutex);
    bool resetBuffer = false;

    if (params.contains("spatial_bin_x")) {
        m_spatialBinX = qMax(1, params.value("spatial_bin_x").toInt());
    }
    if (params.contains("spatial_bin_y")) {
        m_spatialBinY = qMax(1, params.value("spatial_bin_y").toInt());
    }
    if (params.contains("temporal_bin")) {
        const int temporalBin = qMax(1, params.value("temporal_bin").toInt());
        if (temporalBin != m_temporalBin) {
            m_temporalBin = temporalBin;
            resetBuffer = true;
        }
    }
    if (params.contains("spatial_mode")) {
        m_spatialMode = static_cast<BinningMode>(params.value("spatial_mode").toInt());
    }
    if (params.contains("temporal_mode")) {
        const BinningMode temporalMode = static_cast<BinningMode>(params.value("temporal_mode").toInt());
        if (temporalMode != m_temporalMode) {
            m_temporalMode = temporalMode;
            resetBuffer = true;
        }
    }

    if (resetBuffer) {
        m_frameBuffers.clear();
    }
}

} // namespace scopeone::core::internal
