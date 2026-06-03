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

void accumulateFrameSum(const ImageFrame& frame, std::vector<int>& sum, int sign)
{
    dispatchFrameType(frame, [&]<typename Pixel>()
    {
        for (int y = 0; y < frame.height; ++y) {
            const Pixel* row = frameRowData<Pixel>(frame, y);
            const int rowOffset = y * frame.width;
            for (int x = 0; x < frame.width; ++x) {
                sum[static_cast<size_t>(rowOffset + x)] += sign * static_cast<int>(row[x]);
            }
        }
    });
}

void addFrameToSum(const ImageFrame& frame, std::vector<int>& sum)
{
    accumulateFrameSum(frame, sum, 1);
}

void subtractFrameFromSum(const ImageFrame& frame, std::vector<int>& sum)
{
    accumulateFrameSum(frame, sum, -1);
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
                                  const ImageFrame& reference,
                                  const std::vector<int>& sumA,
                                  const std::vector<int>& sumB,
                                  int batchSize,
                                  bool normalize)
{
    const int maxValue = reference.maxValue();
    const double centerValue = reference.isMono16() ? 32768.0 : 128.0;
    const double normalizationScale = reference.isMono16() ? 32767.0 : kNormalizedDisplayScale;
    const int pixelCount = width * height;
    QByteArray bytes = dispatchFrameType(reference, [&]<typename Pixel>()
    {
        QByteArray outBytes = allocatePixelBytes<Pixel>(width, height);
        auto* outData = reinterpret_cast<Pixel*>(outBytes.data());
        for (int i = 0; i < pixelCount; ++i) {
            const double sum1 = static_cast<double>(sumA[static_cast<size_t>(i)]);
            const double sum2 = static_cast<double>(sumB[static_cast<size_t>(i)]);
            const double averageDiff = (sum2 - sum1) / static_cast<double>(batchSize);
            double displayValue = averageDiff + centerValue;
            if (normalize) {
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
        ImageFrame workingFrame;
        if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        const QString cameraKey = frameHistoryKey(in.frame);
        QMutexLocker locker(&m_mutex);

        CameraState& state = m_states[cameraKey];
        const bool incompatibleBuffers = (!state.batchA.empty() && !state.batchA.front().isCompatibleWith(workingFrame))
            || (!state.batchB.empty() && !state.batchB.front().isCompatibleWith(workingFrame));
        if (incompatibleBuffers
            || state.width != workingFrame.width
            || state.height != workingFrame.height
            || state.sumA.size() != workingFrame.width * workingFrame.height
            || state.sumB.size() != workingFrame.width * workingFrame.height) {
            resetState(state, workingFrame);
        }

        if (state.batchA.size() < static_cast<size_t>(m_batchSize)) {
            state.batchA.push_back(workingFrame);
            addFrameToSum(workingFrame, state.sumA);
            out.frame = in.frame;
            return true;
        }

        if (state.batchB.size() < static_cast<size_t>(m_batchSize)) {
            state.batchB.push_back(workingFrame);
            addFrameToSum(workingFrame, state.sumB);
            if (state.batchB.size() < static_cast<size_t>(m_batchSize)) {
                out.frame = in.frame;
                return true;
            }

            out.frame = makeDifferentialOutput(in.frame.cameraId,
                                               workingFrame.width,
                                               workingFrame.height,
                                               workingFrame,
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
        state.batchB.push_back(workingFrame);
        addFrameToSum(workingFrame, state.sumB);

        out.frame = makeDifferentialOutput(in.frame.cameraId,
                                           workingFrame.width,
                                           workingFrame.height,
                                           workingFrame,
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
