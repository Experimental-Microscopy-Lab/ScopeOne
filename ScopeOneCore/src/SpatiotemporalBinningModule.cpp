#include "internal/SpatiotemporalBinningModule.h"
#include "internal/FrameBufferUtils.h"

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

int frameSample(const ImageFrame& frame, int x, int y)
{
    return static_cast<int>(
        reinterpret_cast<const uchar*>(frame.bytes.constData() + y * frame.stride)[x]);
}

ImageFrame applyTemporalBinning(const std::deque<ImageFrame>& buffer,
                                SpatiotemporalBinningModule::BinningMode mode)
{
    // Combine buffered frames into one temporal output
    if (buffer.empty()) {
        return {};
    }
    if (buffer.size() == 1 || mode == SpatiotemporalBinningModule::BinningMode::Skip) {
        return buffer.front();
    }

    const int width = buffer.front().width;
    const int height = buffer.front().height;
    QByteArray bytes;
    bytes.resize(width * height);
    uchar* outData = reinterpret_cast<uchar*>(bytes.data());
    std::vector<int> samples(buffer.size());

    for (int y = 0; y < height; ++y) {
        uchar* dstRow = outData + y * width;
        for (int x = 0; x < width; ++x) {
            for (int i = 0; i < static_cast<int>(buffer.size()); ++i) {
                samples[static_cast<size_t>(i)] = frameSample(buffer[static_cast<size_t>(i)], x, y);
            }
            dstRow[x] = static_cast<uchar>(qBound(0, modeValue(mode, samples), 255));
        }
    }

    return makeMono8Frame(buffer.front().cameraId, width, height, std::move(bytes));
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

    QByteArray bytes;
    bytes.resize(width * height);
    uchar* outData = reinterpret_cast<uchar*>(bytes.data());
    std::vector<int> samples;
    samples.reserve(static_cast<size_t>(binX * binY));

    for (int y = 0; y < height; ++y) {
        uchar* dst = outData + y * width;
        for (int x = 0; x < width; ++x) {
            samples.clear();
            for (int yy = 0; yy < binY; ++yy) {
                for (int xx = 0; xx < binX; ++xx) {
                    samples.push_back(frameSample(frame, x * binX + xx, y * binY + yy));
                }
            }
            dst[x] = static_cast<uchar>(qBound(0, modeValue(mode, samples), 255));
        }
    }

    return makeMono8Frame(frame.cameraId, width, height, std::move(bytes));
}

} // namespace

SpatiotemporalBinningModule::SpatiotemporalBinningModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool SpatiotemporalBinningModule::process(const ModuleInput& in, ModuleOutput& out)
{
    // Apply temporal binning before spatial binning
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
        while (static_cast<int>(m_frameBuffer.size()) > m_temporalBin) {
            m_frameBuffer.pop_front();
        }

        if (static_cast<int>(m_frameBuffer.size()) < m_temporalBin) {
            out.frame = in.frame;
        } else {
            const ImageFrame temporal = applyTemporalBinning(m_frameBuffer, m_temporalMode);
            out.frame = applySpatialBinning(temporal, m_spatialBinX, m_spatialBinY, m_spatialMode);
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
    // Reset temporal history when timing changes
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
        m_frameBuffer.clear();
    }
}

} // namespace scopeone::core::internal
