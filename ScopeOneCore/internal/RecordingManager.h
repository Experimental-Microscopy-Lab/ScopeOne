#pragma once

#include "scopeone/ScopeOneCore.h"
#include "internal/MDAManager.h"
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class CMMCore;

namespace scopeone::core::internal {

using scopeone::core::RecordingFormat;
using scopeone::core::SharedFrameHeader;
using scopeone::core::kRecordingPhaseIdle;
using RecordingFrame = scopeone::core::ScopeOneCore::RecordingFrame;
using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;
using RecordingWriterPhase = scopeone::core::ScopeOneCore::RecordingWriterPhase;
using RecordingWriterStatus = scopeone::core::ScopeOneCore::RecordingWriterStatus;

class MultiProcessCameraManager;

class RecordingManager : public QObject
{
    Q_OBJECT

public:
    struct Settings {

        RecordingFormat format{RecordingFormat::Tiff};
        bool streamToDisk{true};
        bool enableCompression{false};
        int compressionLevel{6};
        int framesPerBurst{1};
        bool burstMode{false};
        int targetBursts{1};
        double burstIntervalMs{0.0};
        double mdaIntervalMs{0.0};
        std::vector<QPointF> positions;
        std::vector<double> zPositions;
        MDAOrder order{MDAAxis::Time, MDAAxis::Z, MDAAxis::XY};
        QString saveDir;
        QString baseName;
        bool captureAll{true};
        QString metadataFileName;
        QByteArray sessionMetadataJson;
    };

    explicit RecordingManager(QObject* parent = nullptr);
    ~RecordingManager() override;

    void setMultiProcessCameraManager(MultiProcessCameraManager* mpcm) { m_mpcm = mpcm; }
    void setMMCore(const std::shared_ptr<CMMCore>& core) { m_mmcore = core; }
    void setLatestFrameFetcher(std::function<bool(const QString&, SharedFrameHeader&, QByteArray&)> fetcher)
    {
        m_latestFrameFetcher = std::move(fetcher);
    }

    bool start(const Settings& settings, const QStringList& activeCameraIds);
    void stop();
    void setRecordedMaxBytes(qint64 bytes);
    qint64 recordedMaxBytes() const;

    bool isRecording() const { return m_captureState.isRecording; }

    void onNewRawFrameReady(const QString& cameraId,
                            const SharedFrameHeader& header,
                            const QByteArray& rawData);

    static QString saveSessionToDisk(const std::shared_ptr<RecordingSessionData>& session);

signals:
    void mdaRawFrameReady(const scopeone::core::ImageFrame& frame,
                         const SharedFrameHeader& header);
    void progressChanged(int phase,
                         qint64 frameCurrent,
                         qint64 frameTarget,
                         int burstCurrent,
                         int burstTarget,
                         qint64 waitRemainingMs,
                         int mdaTimeIndex,
                         int mdaTimeCount,
                         int mdaZIndex,
                         int mdaZCount,
                         int mdaPositionIndex,
                         int mdaPositionCount,
                         bool hasXY,
                         double x,
                         double y,
                         bool hasZ,
                         double z);
    void bufferUsageChanged(qint64 pendingWriteBytes);
    void writerStatusChanged(const RecordingWriterStatus& status);
    void recordingStateChanged(bool isRecording);
    void recordingStopped(const std::shared_ptr<RecordingSessionData>& session);

private:
    struct CapturePlan {
        QStringList activeCameraIds;
        RecordingFormat format{RecordingFormat::Tiff};
        bool streamToDisk{true};
        bool enableCompression{false};
        int compressionLevel{6};
        int framesPerBurst{1};
        bool burstMode{false};
        int targetBursts{1};
        double burstIntervalMs{0.0};
        double mdaIntervalMs{0.0};
        std::vector<QPointF> positions;
        std::vector<double> zPositions;
        MDAOrder order{MDAAxis::Time, MDAAxis::Z, MDAAxis::XY};
        QString saveDir;
        QString baseName;
        bool captureAll{true};
        QString metadataFileName;
        QByteArray sessionMetadataJson;
        bool useMda{false};
        bool streamMda{false};
    };

    struct FramePacket {
        enum class Source {
            PreviewStream,
            Mda
        };

        QString cameraId;
        SharedFrameHeader header{};
        QByteArray rawData;
        Source source{Source::PreviewStream};
    };
    struct WriteTask {
        RecordingFrame frame;
    };

    struct CameraOutput {
        QString cameraId;
        QString rawPath;
        QString frameInfoPath;
        QString metadataFileName;
        QFile frameInfoFile;
        void* backend{nullptr};
        int width{0};
        int height{0};
        int bits{0};
        std::deque<WriteTask> writeQueue;
        mutable std::mutex queueMutex;
        std::condition_variable writeCondition;
        std::thread writerThread;
        bool stopRequested{false};
    };
    struct SessionState {
        std::shared_ptr<RecordingSessionData> activeSession;
    };
    struct WriterState {
        size_t recordedMaxBytes{16ull * 1024 * 1024 * 1024};
        size_t pendingWriteBytes{0};
        QHash<QString, std::shared_ptr<CameraOutput>> cameraOutputs;
        mutable std::mutex writeMutex;
        QString writerError;
        RecordingWriterStatus status;
    };
    struct CaptureState {
        QStringList activeCameraIds;
        QHash<QString, quint64> lastFrameIndex;
        QHash<QString, qint64> framesCapturedThisBurst;
        QHash<QString, qint64> framesCapturedTotal;
        bool isRecording{false};
        bool waitingBetweenBursts{false};
        int currentBurst{0};
        int targetBursts{0};
        int framesPerBurst{0};
        bool burstMode{false};
        double burstIntervalMs{0.0};
        QElapsedTimer elapsedTimer;
        qint64 lastBurstEndMs{0};
        int phase{kRecordingPhaseIdle};
        RecordingFormat format{RecordingFormat::Tiff};
        bool streamToDisk{true};
        bool enableCompression{false};
        int compressionLevel{6};
    };
    struct MdaState {
        MDAManager* manager{nullptr};
        bool usingMda{false};
        bool streamMda{false};
        double streamIntervalMs{0.0};
        qint64 lastStreamCaptureMs{0};
        bool signalsConnected{false};
        QString cameraId;
        quint64 frameIndex{0};
        std::vector<QPointF> positions;
        std::vector<double> zPositions;
        int timePoints{0};
        double intervalMs{0.0};
        int burstsRemaining{0};
        MDAOrder order{MDAAxis::Time, MDAAxis::Z, MDAAxis::XY};
        MDAEvent lastEvent{};
        bool hasLastEvent{false};
    };
    bool buildCapturePlan(const Settings& settings,
                          const QStringList& activeCameraIds,
                          CapturePlan& plan,
                          QString& errorMessage) const;
    void resetCaptureState(const CapturePlan& plan);
    void resetSessionState(const CapturePlan& plan);
    void finalizeActiveSession();
    void primeLastFrameIndices();
    void emitProgress();
    bool startStreamingOutputs(const CapturePlan& plan);
    void stopStreamingOutputs();
    void requestWriterStop();
    void writerLoop(const std::shared_ptr<CameraOutput>& output);
    bool writeTask(CameraOutput& output, const WriteTask& task, QString& errorMessage);
    void writeFrameInfoHeader(CameraOutput& output);
    QByteArray buildFrameInfoLine(const QString& cameraId, const RecordingFrame& frame) const;
    QString formatName(RecordingFormat format) const;
    void emitBufferUsageChanged(qint64 pendingWriteBytes);
    void emitWriterStatus();
    void setWriterStatus(RecordingWriterPhase phase, const QString& errorMessage = QString());
    qint64 totalFramesWritten() const;
    bool enqueueFrame(const RecordingFrame& frame, const QString& cameraId);
    bool shouldAcceptFrame(const FramePacket& packet) const;

    void ingestFrame(const FramePacket& packet);
    void handleMdaFrame(const MDAOutput& frame);
    bool startMdaCapture();
    void startMdaRun();

    bool shouldCaptureCamera(const QString& cameraId) const;
    bool allCamerasReachedTarget() const;
    bool advanceBurstStateIfNeeded();

    MultiProcessCameraManager* m_mpcm{nullptr};
    std::shared_ptr<CMMCore> m_mmcore;
    std::function<bool(const QString&, SharedFrameHeader&, QByteArray&)> m_latestFrameFetcher;

    SessionState m_sessionState;
    WriterState m_writerState;
    CaptureState m_captureState;
    MdaState m_mdaState;
};

}
