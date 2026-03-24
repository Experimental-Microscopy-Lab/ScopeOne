#include "internal/MDAManager.h"
#include "internal/MultiProcessCameraManager.h"
#include "MMCore.h"

#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QDateTime>
#include <algorithm>
#include <functional>
#include <future>
#include <vector>

namespace scopeone::core::internal {

using scopeone::core::SharedFrameHeader;
using scopeone::core::SharedPixelFormat;

MDAManager::MDAManager(std::shared_ptr<CMMCore> core, QObject* parent)
    : QObject(parent)
    , m_mmcore(std::move(core))
{
    qRegisterMetaType<scopeone::core::internal::MDAEvent>("scopeone::core::internal::MDAEvent");
    qRegisterMetaType<scopeone::core::internal::CameraFrame>("scopeone::core::internal::CameraFrame");
    qRegisterMetaType<scopeone::core::internal::MDAOutput>("scopeone::core::internal::MDAOutput");
}

void MDAManager::setCameras(const QStringList& cameraIds)
{
    m_cameraIds = cameraIds;
}

void MDAManager::setMultiProcessCameraManager(MultiProcessCameraManager* mpcm)
{
    m_mpcm = mpcm;
}

void MDAManager::start(int timePoints,
                       double timeIntervalMs,
                       double exposureMs,
                       const std::vector<QPointF>& positions,
                       const std::vector<double>& zPositions,
                       const MDAOrder& order,
                       bool block)
{
    // Start one MDA run on demand
    if (m_running.load()) {
        emit sequenceError(QStringLiteral("MDA already running"));
        return;
    }

    m_cancelRequested.store(false);
    m_running.store(true);

    if (block) {
        runSequence(timePoints, timeIntervalMs, exposureMs, positions, zPositions, order);
        return;
    }

    QThreadPool::globalInstance()->start([this, timePoints, timeIntervalMs, exposureMs, positions, zPositions, order]() {
        runSequence(timePoints, timeIntervalMs, exposureMs, positions, zPositions, order);
    });
}

void MDAManager::requestCancel()
{
    m_cancelRequested.store(true);
}

void MDAManager::runSequence(int timePoints,
                             double timeIntervalMs,
                             double exposureMs,
                             std::vector<QPointF> positions,
                             std::vector<double> zPositions,
                             MDAOrder order)
{
    // Expand the event order into one timed sequence
    QString error;

    if (!m_mmcore) {
        emit sequenceError(QStringLiteral("MMCore not available"));
        m_running.store(false);
        return;
    }

    QElapsedTimer timer;
    timer.start();

    const int tCount = (timePoints < 1) ? 1 : timePoints;
    const bool hasXY = !positions.empty();
    const bool hasZ = !zPositions.empty();
    const int posCount = hasXY ? positions.size() : 1;
    const int zCount = hasZ ? zPositions.size() : 1;
    if (order.empty()) {
        order = MDAOrder{MDAAxis::Time, MDAAxis::Z, MDAAxis::XY};
    }

    auto runEvent = [&](int t, int p, int z, const QPointF& pos, double zPos, qint64 tOffsetMs) {
        if (m_cancelRequested.load()) {
            emit sequenceCanceled();
            m_running.store(false);
            return false;
        }

        const qint64 targetMs = tOffsetMs;
        if (targetMs > 0) {
            while (timer.elapsed() < targetMs) {
                if (m_cancelRequested.load()) {
                    emit sequenceCanceled();
                    m_running.store(false);
                    return false;
                }
                const qint64 remaining = targetMs - timer.elapsed();
                if (remaining > 0) {
                    QThread::msleep(static_cast<unsigned long>((std::min<qint64>)(5, remaining)));
                }
            }
        }

        MDAEvent ev;
        ev.tIndex = t;
        ev.positionIndex = p;
        ev.zIndex = z;
        ev.x = pos.x();
        ev.y = pos.y();
        ev.z = zPos;
        ev.hasXY = hasXY && posCount > 1;
        ev.hasZ = hasZ && zCount > 1;
        ev.exposureMs = exposureMs;
        ev.minStartTimeMs = tOffsetMs;

        if (!setupEvent(ev, &error)) {
            emit sequenceError(error.isEmpty() ? QStringLiteral("Failed to setup event") : error);
            m_running.store(false);
            return false;
        }

        MDAOutput frame;
        if (!execEvent(ev, frame, &error)) {
            emit sequenceError(error.isEmpty() ? QStringLiteral("Failed to execute event") : error);
            m_running.store(false);
            return false;
        }
        emit frameReady(frame);
        return true;
    };

    int tIndex = 0;
    int zIndex = 0;
    int pIndex = 0;

    auto axisCount = [&](MDAAxis axis) -> int {
        switch (axis) {
        case MDAAxis::Time:
            return tCount;
        case MDAAxis::Z:
            return zCount;
        case MDAAxis::XY:
            return posCount;
        default:
            return 1;
        }
    };

    std::function<bool(int)> loop = [&](int depth) -> bool {
        if (depth >= order.size()) {
            const QPointF pos = hasXY ? positions.at(pIndex) : QPointF(0.0, 0.0);
            const double zPos = hasZ ? zPositions.at(zIndex) : 0.0;
            const qint64 tOffsetMs = static_cast<qint64>(tIndex * timeIntervalMs);
            return runEvent(tIndex, pIndex, zIndex, pos, zPos, tOffsetMs);
        }

        const MDAAxis axis = order.at(depth);
        const int count = axisCount(axis);
        for (int i = 0; i < count; ++i) {
            switch (axis) {
            case MDAAxis::Time:
                tIndex = i;
                break;
            case MDAAxis::Z:
                zIndex = i;
                break;
            case MDAAxis::XY:
                pIndex = i;
                break;
            default:
                break;
            }
            if (!loop(depth + 1)) {
                return false;
            }
        }
        return true;
    };

    if (!loop(0)) {
        return;
    }

    emit sequenceFinished();
    m_running.store(false);
}

bool MDAManager::setupEvent(const MDAEvent& event, QString* errorMessage)
{
    // Move hardware into place before capture
    if (!m_mmcore) {
        if (errorMessage) *errorMessage = QStringLiteral("MMCore not available");
        return false;
    }

    if (event.exposureMs > 0.0) {
        if (!setExposure(event.exposureMs, errorMessage)) {
            return false;
        }
    }
    if (event.hasXY) {
        if (!moveXY(event.x, event.y, errorMessage)) {
            return false;
        }
    }
    if (event.hasZ) {
        if (!moveZ(event.z, errorMessage)) {
            return false;
        }
    }

    try {
        m_mmcore->waitForSystem();
    } catch (const CMMError& e) {
        if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
        return false;
    }

    return true;
}

bool MDAManager::execEvent(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage)
{
    // Route to appropriate implementation based on camera configuration
    if (!m_cameraIds.isEmpty() && m_mpcm) {
        return execEventMultiCamera(event, outFrame, errorMessage);
    } else {
        return execEventSingleCamera(event, outFrame, errorMessage);
    }
}

bool MDAManager::execEventSingleCamera(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage)
{
    if (!m_mmcore) {
        if (errorMessage) *errorMessage = QStringLiteral("MMCore not available");
        return false;
    }

    try {
        m_mmcore->snapImage();

        const unsigned width = m_mmcore->getImageWidth();
        const unsigned height = m_mmcore->getImageHeight();
        const unsigned bytesPerPixel = m_mmcore->getBytesPerPixel();
        const unsigned byteCount = width * height * bytesPerPixel;
        const int bitDepth = static_cast<int>(m_mmcore->getImageBitDepth());

        const unsigned char* ptr = static_cast<unsigned char*>(m_mmcore->getImage());
        if (!ptr || byteCount == 0) {
            if (errorMessage) *errorMessage = QStringLiteral("Empty image buffer");
            return false;
        }

        outFrame.event = event;
        outFrame.width = static_cast<int>(width);
        outFrame.height = static_cast<int>(height);
        outFrame.bytesPerPixel = static_cast<int>(bytesPerPixel);
        outFrame.bitsPerSample = (bitDepth > 0) ? bitDepth : static_cast<int>(bytesPerPixel * 8);
        outFrame.timestampMs = QDateTime::currentMSecsSinceEpoch();
        outFrame.raw = QByteArray(reinterpret_cast<const char*>(ptr), static_cast<int>(byteCount));
        return true;
    } catch (const CMMError& e) {
        if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
        return false;
    }
}

bool MDAManager::execEventMultiCamera(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage)
{
    // Trigger all active cameras for one MDA event
    if (!m_mpcm) {
        if (errorMessage) *errorMessage = QStringLiteral("MultiProcessCameraManager not available");
        return false;
    }

    outFrame.event = event;
    outFrame.timestampMs = QDateTime::currentMSecsSinceEpoch();
    outFrame.frames.clear();
    const int captureTimeoutMs = static_cast<int>((std::max)(1500.0, event.exposureMs * 4.0 + 500.0));

    struct CaptureResult {
        QString cameraId;
        SharedFrameHeader header{};
        QByteArray data;
        bool ok{false};
        QString error;
    };

    std::vector<std::future<CaptureResult>> futures;
    futures.reserve(static_cast<size_t>(m_cameraIds.size()));

    // Trigger all cameras in parallel for this MDA event
    for (const QString& cameraId : m_cameraIds) {
        futures.emplace_back(std::async(std::launch::async, [this, cameraId, captureTimeoutMs]() {
            CaptureResult result;
            result.cameraId = cameraId;
            if (!m_mpcm->captureEventFrame(cameraId, result.header, result.data, captureTimeoutMs)) {
                result.error = QString("Failed to capture frame from camera: %1").arg(cameraId);
                return result;
            }
            if (result.data.isEmpty()) {
                result.error = QString("Empty frame from camera: %1").arg(cameraId);
                return result;
            }
            result.ok = true;
            return result;
        }));
    }

    for (auto& future : futures) {
        const CaptureResult result = future.get();
        if (!result.ok) {
            if (errorMessage) *errorMessage = result.error;
            return false;
        }

        CameraFrame frame;
        frame.cameraId = result.cameraId;
        frame.raw = result.data;
        frame.width = static_cast<int>(result.header.width);
        frame.height = static_cast<int>(result.header.height);
        frame.bytesPerPixel = (result.header.pixelFormat == static_cast<quint32>(SharedPixelFormat::Mono16)) ? 2 : 1;
        frame.bitsPerSample = static_cast<int>(result.header.bitsPerSample);
        outFrame.frames.insert(result.cameraId, frame);
    }

    // Legacy compatibility: if single camera, also populate old fields
    if (outFrame.frames.size() == 1) {
        const CameraFrame& frame = outFrame.frames.first();
        outFrame.raw = frame.raw;
        outFrame.width = frame.width;
        outFrame.height = frame.height;
        outFrame.bytesPerPixel = frame.bytesPerPixel;
        outFrame.bitsPerSample = frame.bitsPerSample;
    }

    return true;
}

bool MDAManager::setExposure(double exposureMs, QString* errorMessage)
{
    try {
        m_mmcore->setExposure(exposureMs);
        return true;
    } catch (const CMMError& e) {
        if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
        return false;
    }
}

bool MDAManager::moveXY(double x, double y, QString* errorMessage)
{
    try {
        const std::string xyStage = m_mmcore->getXYStageDevice();
        if (xyStage.empty()) {
            if (errorMessage) *errorMessage = QStringLiteral("No XY stage device configured");
            return false;
        }
        m_mmcore->setXYPosition(xyStage.c_str(), x, y);
        return true;
    } catch (const CMMError& e) {
        if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
        return false;
    }
}

bool MDAManager::moveZ(double z, QString* errorMessage)
{
    try {
        const std::string focus = m_mmcore->getFocusDevice();
        if (focus.empty()) {
            if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
            return false;
        }
        m_mmcore->setPosition(focus.c_str(), z);
        return true;
    } catch (const CMMError& e) {
        if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
        return false;
    }
}

} // namespace scopeone::core::internal
