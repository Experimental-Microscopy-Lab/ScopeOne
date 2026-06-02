#include "internal/DifferentialRollingModule.h"

#include "internal/FrameBufferUtils.h"

#include <QMutexLocker>

namespace scopeone::core::internal {

namespace {

constexpr double kNormalizedDisplayScale = 4096.0;
constexpr double kNormalizationEpsilon = 1.0;

QString frameHistoryKey(const ImageFrame& frame)
{
    return frame.cameraId.isEmpty() ? QStringLiteral("__default__") : frame.cameraId;
}

void addFrameToSum(const ImageFrame& frame, std::vector<int>& sum)
{
    for (int y = 0; y < frame.height; ++y) {
        const uchar* row = reinterpret_cast<const uchar*>(frame.bytes.constData() + y * frame.stride);
        const int rowOffset = y * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            sum[static_cast<size_t>(rowOffset + x)] += static_cast<int>(row[x]);
        }
    }
}

void subtractFrameFromSum(const ImageFrame& frame, std::vector<int>& sum)
{
    for (int y = 0; y < frame.height; ++y) {
        const uchar* row = reinterpret_cast<const uchar*>(frame.bytes.constData() + y * frame.stride);
        const int rowOffset = y * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            sum[static_cast<size_t>(rowOffset + x)] -= static_cast<int>(row[x]);
        }
    }
}

void resetState(DifferentialRollingModule::CameraState& state, const ImageFrame& frame)
{
    state = DifferentialRollingModule::CameraState{};
    state.width = frame.width;
    state.height = frame.height;
    state.sumA.assign(static_cast<size_t>(frame.width * frame.height), 0);
    state.sumB.assign(static_cast<size_t>(frame.width * frame.height), 0);
}

ImageFrame makeDifferentialOutput(const QString& cameraId,
                                  int width,
                                  int height,
                                  const std::vector<int>& sumA,
                                  const std::vector<int>& sumB,
                                  int batchSize,
                                  bool normalize)
{
    QByteArray bytes;
    bytes.resize(width * height);
    uchar* outData = reinterpret_cast<uchar*>(bytes.data());

    const int pixelCount = width * height;
    for (int i = 0; i < pixelCount; ++i) {
        const double sum1 = static_cast<double>(sumA[static_cast<size_t>(i)]);
        const double sum2 = static_cast<double>(sumB[static_cast<size_t>(i)]);
        const double averageDiff = (sum2 - sum1) / static_cast<double>(batchSize);
        double displayValue = averageDiff + 128.0;
        if (normalize) {
            const double normalized = (sum2 - sum1)
                                      / (sum1 > static_cast<double>(batchSize)
                                             ? sum1
                                             : static_cast<double>(batchSize) * kNormalizationEpsilon);
            displayValue = 128.0 + normalized * kNormalizedDisplayScale;
        }
        outData[i] = static_cast<uchar>(qBound(0, qRound(displayValue), 255));
    }

    return makeMono8Frame(cameraId, width, height, std::move(bytes));
}

} // namespace

DifferentialRollingModule::DifferentialRollingModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool DifferentialRollingModule::process(const ModuleInput& in, ModuleOutput& out)
{
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

        const QString cameraKey = frameHistoryKey(in.frame);
        QMutexLocker locker(&m_mutex);

        CameraState& state = m_states[cameraKey];
        if (state.width != mono8Frame.width
            || state.height != mono8Frame.height
            || state.sumA.size() != mono8Frame.width * mono8Frame.height
            || state.sumB.size() != mono8Frame.width * mono8Frame.height) {
            resetState(state, mono8Frame);
        }

        if (state.batchA.size() < static_cast<size_t>(m_batchSize)) {
            state.batchA.push_back(mono8Frame);
            addFrameToSum(mono8Frame, state.sumA);
            out.frame = in.frame;
            return true;
        }

        if (state.batchB.size() < static_cast<size_t>(m_batchSize)) {
            state.batchB.push_back(mono8Frame);
            addFrameToSum(mono8Frame, state.sumB);
            if (state.batchB.size() < static_cast<size_t>(m_batchSize)) {
                out.frame = in.frame;
                return true;
            }

            out.frame = makeDifferentialOutput(in.frame.cameraId,
                                               mono8Frame.width,
                                               mono8Frame.height,
                                               state.sumA,
                                               state.sumB,
                                               m_batchSize,
                                               m_normalize);
            return true;
        }

        const ImageFrame oldestA = state.batchA.front();
        const ImageFrame bridgeFrame = state.batchB.front();

        subtractFrameFromSum(oldestA, state.sumA);
        state.batchA.pop_front();
        state.batchA.push_back(bridgeFrame);
        addFrameToSum(bridgeFrame, state.sumA);

        subtractFrameFromSum(bridgeFrame, state.sumB);
        state.batchB.pop_front();
        state.batchB.push_back(mono8Frame);
        addFrameToSum(mono8Frame, state.sumB);

        out.frame = makeDifferentialOutput(in.frame.cameraId,
                                           mono8Frame.width,
                                           mono8Frame.height,
                                           state.sumA,
                                           state.sumB,
                                           m_batchSize,
                                           m_normalize);
        return true;
    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Differential rolling failed: %1").arg(e.what());
        return false;
    }
}

QVariantMap DifferentialRollingModule::getParameters() const
{
    QVariantMap params;
    params["batch_size"] = m_batchSize;
    params["normalize"] = m_normalize;
    return params;
}

void DifferentialRollingModule::setParameters(const QVariantMap& params)
{
    QMutexLocker locker(&m_mutex);
    bool resetState = false;

    if (params.contains("batch_size")) {
        const int batchSize = qMax(1, params.value("batch_size").toInt());
        if (batchSize != m_batchSize) {
            m_batchSize = batchSize;
            resetState = true;
        }
    }
    if (params.contains("normalize")) {
        m_normalize = params.value("normalize").toBool();
    }

    if (resetState) {
        m_states.clear();
    }
}

void DifferentialRollingModule::resetBuffer()
{
    QMutexLocker locker(&m_mutex);
    m_states.clear();
}

} // namespace scopeone::core::internal
