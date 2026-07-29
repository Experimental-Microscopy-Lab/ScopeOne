#include "internal/StageMosaicManager.h"

#include "internal/FrameBufferUtils.h"

#include <QDateTime>
#include <QTimer>
#include <opencv2/core.hpp>
#include <cmath>
#include <limits>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr qsizetype kMaxMosaicPixels = 10000ll * 10000ll;
        constexpr int kFrameTimeoutMs = 2000;
        const QString kMosaicLayerId = QStringLiteral("stage_mosaic");

        cv::Mat frameMatView(const ImageFrame& frame)
        {
            if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16()))
            {
                return {};
            }
            return cv::Mat(frame.height,
                           frame.width,
                           frame.isMono16() ? CV_16UC1 : CV_8UC1,
                           const_cast<char*>(frame.bytes.constData()),
                           static_cast<size_t>(frame.stride));
        }
    }

    class StageMosaicManager::Storage
    {
    public:
        cv::Mat sum;
        cv::Mat count;
        int tileWidth{0};
        int tileHeight{0};
        int outputType{CV_8UC1};
        double minOffsetXUm{0.0};
        double minOffsetYUm{0.0};
    };

    StageMosaicManager::StageMosaicManager(ScopeOneCore* core, QObject* parent)
        : QObject(parent),
          m_core(core),
          m_storage(std::make_unique<Storage>())
    {
        connect(core, &ScopeOneCore::newRawFrameReady,
                this, &StageMosaicManager::handleRawFrame);
        connect(core, &ScopeOneCore::stageMoveFinished,
                this, [this](quint64 commandId, const QString&, bool success, const QString& errorMessage)
                {
                    handleStageMoveFinished(commandId, success, errorMessage);
                });
    }

    StageMosaicManager::~StageMosaicManager() = default;

    bool StageMosaicManager::start(const ScopeOneCore::StageMosaicPlan& plan,
                                   QString* errorMessage)
    {
        if (isRunning())
        {
            if (errorMessage) *errorMessage = QStringLiteral("A stage mosaic is already running");
            return false;
        }
        if (m_core->isRecording() || !m_core->activeExperimentId().isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Another acquisition is already running");
            return false;
        }

        ScopeOneCore::StageMosaicPlan normalized = plan;
        normalized.cameraId = normalized.cameraId.trimmed();
        normalized.xyStageId = normalized.xyStageId.trimmed();
        normalized.gallerySaveDir = normalized.gallerySaveDir.trimmed();
        const qint64 tileCount = static_cast<qint64>(normalized.rows) * normalized.columns;
        if (normalized.cameraId.isEmpty()
            || normalized.xyStageId.isEmpty()
            || !m_core->cameraIds().contains(normalized.cameraId)
            || !m_core->xyStageDevices().contains(normalized.xyStageId)
            || normalized.rows <= 0
            || normalized.columns <= 0
            || tileCount <= 0
            || tileCount > 10000
            || !std::isfinite(normalized.pixelSizeUm)
            || normalized.pixelSizeUm <= 0.0
            || !std::isfinite(normalized.stepXUm)
            || !std::isfinite(normalized.stepYUm)
            || normalized.settleMs < 0)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Invalid stage mosaic plan");
            return false;
        }
        if (!m_core->readXYPosition(normalized.xyStageId, m_originX, m_originY))
        {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to read current stage position");
            return false;
        }

        m_startedPreview = !m_core->runningPreviewCameraIds().contains(normalized.cameraId);
        if (m_startedPreview && !m_core->startPreview(normalized.cameraId))
        {
            m_startedPreview = false;
            if (errorMessage) *errorMessage = QStringLiteral("Failed to start camera preview");
            return false;
        }

        m_plan = normalized;
        m_currentTile = 0;
        m_frameWaitSerial = 0;
        m_waitingForFrame = false;
        m_pendingMoveCommandId = 0;
        m_referenceFrame = ImageFrame{};
        m_storage->sum.release();
        m_storage->count.release();
        m_storage->tileWidth = 0;
        m_storage->tileHeight = 0;
        m_status = ScopeOneCore::StageMosaicStatus{};
        m_status.state = ScopeOneCore::StageMosaicState::Running;
        const quint64 generation = ++m_generation;
        reportProgress(0, static_cast<int>(tileCount), QStringLiteral("Starting mosaic capture"));
        QTimer::singleShot(qMax(50, m_plan.settleMs), this,
                           [this, generation]() { captureNextTile(generation); });
        return true;
    }

    void StageMosaicManager::cancel()
    {
        if (isRunning())
        {
            finish(false, true, QStringLiteral("Mosaic stopped"));
        }
    }

    void StageMosaicManager::captureNextTile(quint64 generation)
    {
        if (!isRunning() || generation != m_generation)
        {
            return;
        }
        const int totalTiles = m_plan.rows * m_plan.columns;
        if (m_currentTile >= totalTiles)
        {
            finish(true, false, QStringLiteral("Mosaic complete with averaged overlap"));
            return;
        }

        const int row = m_currentTile / m_plan.columns;
        const int column = m_currentTile % m_plan.columns;
        const double x = m_originX + column * m_plan.stepXUm;
        const double y = m_originY + row * m_plan.stepYUm;
        reportProgress(m_currentTile,
                       totalTiles,
                       QStringLiteral("Moving to tile %1 of %2")
                           .arg(m_currentTile + 1)
                           .arg(totalTiles));
        m_pendingMoveCommandId = m_core->moveXYTo(m_plan.xyStageId, x, y);
        if (m_pendingMoveCommandId == 0)
        {
            finish(false, false, QStringLiteral("Stage move failed"));
        }
    }

    // Continues mosaic capture after the asynchronous stage move
    void StageMosaicManager::handleStageMoveFinished(quint64 commandId,
                                                     bool success,
                                                     const QString& errorMessage)
    {
        if (!isRunning() || commandId == 0 || commandId != m_pendingMoveCommandId)
        {
            return;
        }
        m_pendingMoveCommandId = 0;
        if (!success)
        {
            finish(false,
                   false,
                   errorMessage.isEmpty() ? QStringLiteral("Stage move failed") : errorMessage);
            return;
        }

        const quint64 generation = m_generation;
        QTimer::singleShot(m_plan.settleMs, this,
                           [this, generation]() { waitForTileFrame(generation); });
    }

    void StageMosaicManager::waitForTileFrame(quint64 generation)
    {
        if (!isRunning() || generation != m_generation)
        {
            return;
        }
        m_waitingForFrame = true;
        const int waitSerial = ++m_frameWaitSerial;
        reportProgress(m_currentTile,
                       m_plan.rows * m_plan.columns,
                       QStringLiteral("Waiting for camera frame"));
        QTimer::singleShot(kFrameTimeoutMs, this, [this, generation, waitSerial]()
        {
            if (isRunning()
                && generation == m_generation
                && m_waitingForFrame
                && waitSerial == m_frameWaitSerial)
            {
                finish(false, false, QStringLiteral("Timed out waiting for camera frame"));
            }
        });
    }

    void StageMosaicManager::handleRawFrame(const ImageFrame& frame)
    {
        if (!isRunning() || !m_waitingForFrame || frame.cameraId != m_plan.cameraId)
        {
            return;
        }
        m_waitingForFrame = false;
        m_pendingMoveCommandId = 0;

        QString errorMessage;
        if (m_storage->sum.empty() && !initializeMosaic(frame, errorMessage))
        {
            finish(false, false, errorMessage);
            return;
        }
        const int row = m_currentTile / m_plan.columns;
        const int column = m_currentTile % m_plan.columns;
        if (!appendTile(frame, row, column))
        {
            finish(false, false, QStringLiteral("Tile frame did not match the mosaic format"));
            return;
        }

        const ImageFrame mosaicFrame = publishMosaicFrame();
        if (!mosaicFrame.isValid())
        {
            finish(false, false, QStringLiteral("Failed to publish mosaic preview"));
            return;
        }
        emit frameUpdated(mosaicFrame);
        ++m_currentTile;
        reportProgress(m_currentTile,
                       m_plan.rows * m_plan.columns,
                       QStringLiteral("Captured tile %1 of %2")
                           .arg(m_currentTile)
                           .arg(m_plan.rows * m_plan.columns));
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this,
                           [this, generation]() { captureNextTile(generation); });
    }

    bool StageMosaicManager::initializeMosaic(const ImageFrame& frame, QString& errorMessage)
    {
        const cv::Mat tile = frameMatView(frame);
        if (tile.empty())
        {
            errorMessage = QStringLiteral("Unsupported live frame format");
            return false;
        }

        const double lastOffsetXUm = (m_plan.columns - 1) * m_plan.stepXUm;
        const double lastOffsetYUm = (m_plan.rows - 1) * m_plan.stepYUm;
        const double offsetXPixels = qAbs(lastOffsetXUm) / m_plan.pixelSizeUm;
        const double offsetYPixels = qAbs(lastOffsetYUm) / m_plan.pixelSizeUm;
        const double widthValue = tile.cols + offsetXPixels;
        const double heightValue = tile.rows + offsetYPixels;
        if (!std::isfinite(widthValue)
            || !std::isfinite(heightValue)
            || widthValue > (std::numeric_limits<int>::max)()
            || heightValue > (std::numeric_limits<int>::max)()
            || widthValue * heightValue > kMaxMosaicPixels)
        {
            errorMessage = QStringLiteral("Mosaic output is too large");
            return false;
        }

        const int width = tile.cols + qRound(offsetXPixels);
        const int height = tile.rows + qRound(offsetYPixels);
        if (static_cast<qsizetype>(width) * height > kMaxMosaicPixels)
        {
            errorMessage = QStringLiteral("Mosaic output is too large");
            return false;
        }
        m_storage->tileWidth = tile.cols;
        m_storage->tileHeight = tile.rows;
        m_storage->outputType = tile.type();
        m_storage->minOffsetXUm = qMin(0.0, lastOffsetXUm);
        m_storage->minOffsetYUm = qMin(0.0, lastOffsetYUm);
        m_storage->sum = cv::Mat::zeros(height, width, CV_32FC1);
        m_storage->count = cv::Mat::zeros(height, width, CV_32FC1);
        return true;
    }

    bool StageMosaicManager::appendTile(const ImageFrame& frame, int row, int column)
    {
        const cv::Mat tile = frameMatView(frame);
        if (tile.empty()
            || tile.cols != m_storage->tileWidth
            || tile.rows != m_storage->tileHeight
            || tile.type() != m_storage->outputType)
        {
            return false;
        }
        const int x = qRound((column * m_plan.stepXUm - m_storage->minOffsetXUm)
                             / m_plan.pixelSizeUm);
        const int y = qRound((row * m_plan.stepYUm - m_storage->minOffsetYUm)
                             / m_plan.pixelSizeUm);
        if (x < 0
            || y < 0
            || x + tile.cols > m_storage->sum.cols
            || y + tile.rows > m_storage->sum.rows)
        {
            return false;
        }

        const cv::Rect roi(x, y, tile.cols, tile.rows);
        cv::Mat tileFloat;
        tile.convertTo(tileFloat, CV_32F);
        m_storage->sum(roi) += tileFloat;
        m_storage->count(roi) += cv::Scalar(1.0);
        m_referenceFrame = frame;
        return true;
    }

    ImageFrame StageMosaicManager::publishMosaicFrame()
    {
        cv::Mat safeCount;
        cv::max(m_storage->count, cv::Scalar(1.0), safeCount);
        cv::Mat average;
        cv::divide(m_storage->sum, safeCount, average);
        cv::Mat output;
        average.convertTo(output, m_storage->outputType);
        output.setTo(cv::Scalar(0), m_storage->count == 0);

        ImageFrame mosaicFrame = makeFrameLike(m_referenceFrame,
                                               output.cols,
                                               output.rows,
                                               copyMatBytes(output));
        mosaicFrame.sourceRoiX = 0;
        mosaicFrame.sourceRoiY = 0;
        mosaicFrame.sourceRoiWidth = mosaicFrame.width;
        mosaicFrame.sourceRoiHeight = mosaicFrame.height;
        return m_core->publishStaticFrame(
            kMosaicLayerId,
            mosaicFrame,
            QStringLiteral("Stage Mosaic %1").arg(m_plan.cameraId));
    }

    void StageMosaicManager::reportProgress(int completedTiles,
                                            int totalTiles,
                                            const QString& message)
    {
        m_status.completedTiles = completedTiles;
        m_status.totalTiles = totalTiles;
        m_status.message = message;
        emit progressChanged(completedTiles, totalTiles, message);
    }

    void StageMosaicManager::finish(bool success, bool canceled, const QString& message)
    {
        if (!isRunning())
        {
            return;
        }
        m_waitingForFrame = false;
        ++m_generation;
        if (canceled)
        {
            m_status.state = ScopeOneCore::StageMosaicState::Canceled;
        }
        else if (success)
        {
            m_status.state = ScopeOneCore::StageMosaicState::Completed;
        }
        else
        {
            m_status.state = ScopeOneCore::StageMosaicState::Failed;
        }
        std::shared_ptr<ScopeOneCore::RecordingSessionData> session;
        QString finalMessage = message;
        if (success)
        {
            const ImageFrame frame = m_core->graphFrame(ScopeOneCore::staticLayerKey(kMosaicLayerId));
            ExperimentPlan capturePlan;
            capturePlan.cameraIds = {frame.cameraId};
            capturePlan.streamToDisk = false;
            capturePlan.format = RecordingFormat::OmeTiff;
            capturePlan.pixelSizeUm = m_plan.pixelSizeUm;
            capturePlan.saveDir = m_plan.gallerySaveDir;
            capturePlan.baseName = QStringLiteral("stage_mosaic_")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
            capturePlan.metadataFileName = capturePlan.baseName + QStringLiteral("_metadata.json");
            session = m_core->createFrameSession({frame}, capturePlan);
            if (!session)
            {
                finalMessage = QStringLiteral("Mosaic completed but the gallery session could not be created");
                m_status.state = ScopeOneCore::StageMosaicState::Failed;
            }
            else
            {
                m_status.sessionId = session->capturePlan().experimentId;
            }
        }

        if (m_plan.returnToStart)
        {
            m_core->moveXYTo(m_plan.xyStageId, m_originX, m_originY);
        }
        if (m_startedPreview)
        {
            m_core->stopPreview(m_plan.cameraId);
        }
        m_startedPreview = false;
        reportProgress(m_currentTile, m_plan.rows * m_plan.columns, finalMessage);
        emit finished(session, finalMessage, canceled);
    }
}
