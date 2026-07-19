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

namespace scopeone::core::internal
{
    using scopeone::core::RecordingFormat;
    using scopeone::core::ExperimentPlan;
    using scopeone::core::ExperimentRunState;
    using scopeone::core::ImageFrame;
    using scopeone::core::kRecordingPhaseIdle;
    using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;
    using RecordingWriterPhase = scopeone::core::ScopeOneCore::RecordingWriterPhase;
    using RecordingWriterStatus = scopeone::core::ScopeOneCore::RecordingWriterStatus;

    class MultiProcessCameraManager;

    class RecordingManager : public QObject
    {
        Q_OBJECT

    public:
        explicit RecordingManager(QObject* parent = nullptr);
        ~RecordingManager() override;

        void setMultiProcessCameraManager(MultiProcessCameraManager* mpcm) { m_mpcm = mpcm; }
        void setMMCore(const std::shared_ptr<CMMCore>& core) { m_mmcore = core; }

        void setLatestFrameFetcher(std::function<bool(const QString&, ImageFrame&)> fetcher)
        {
            m_latestFrameFetcher = std::move(fetcher);
        }

        bool start(const ExperimentPlan& requestedPlan,
                   const QStringList& activeCameraIds,
                   const QJsonObject& deviceProperties);
        void stop();
        void setRecordedMaxBytes(qint64 bytes);
        qint64 recordedMaxBytes() const;

        bool isRecording() const { return m_captureState.isRecording; }

        void onNewRawFrameReady(const ImageFrame& frame);

        static QString saveSessionToDisk(const std::shared_ptr<RecordingSessionData>& session);

    signals:
        void mdaRawFrameReady(const scopeone::core::ImageFrame& frame);
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
        struct FramePacket
        {
            enum class Source
            {
                PreviewStream,
                Mda
            };

            ImageFrame frame;
            Source source{Source::PreviewStream};
        };

        struct WriteTask
        {
            ImageFrame frame;
        };

        struct CameraOutput
        {
            QString cameraId;
            QString rawPath;
            QString frameInfoPath;
            QString metadataFileName;
            QFile frameInfoFile;
            void* backend{nullptr};
            int width{0};
            int height{0};
            int bits{0};
            scopeone::core::ImagePixelFormat pixelFormat{scopeone::core::ImagePixelFormat::Invalid};
            std::deque<WriteTask> writeQueue;
            mutable std::mutex queueMutex;
            std::condition_variable writeCondition;
            std::thread writerThread;
            bool stopRequested{false};
            qint64 framesWritten{0};
        };

        struct SessionState
        {
            std::shared_ptr<RecordingSessionData> activeSession;
        };

        struct WriterState
        {
            size_t recordedMaxBytes{16ull * 1024 * 1024 * 1024};
            size_t pendingWriteBytes{0};
            QHash<QString, std::shared_ptr<CameraOutput>> cameraOutputs;
            mutable std::mutex writeMutex;
            QString writerError;
            RecordingWriterStatus status;
        };

        struct CaptureState
        {
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
            quint64 generation{0};
        };

        struct MdaState
        {
            MDAManager* manager{nullptr};
            bool usingMda{false};
            bool streamMda{false};
            double streamIntervalMs{0.0};
            qint64 lastStreamCaptureMs{0};
            QString cameraId;
            quint64 frameIndex{0};
            int burstsRemaining{0};
            ExperimentPlan plan;
            AcquisitionEvent lastEvent{};
            bool hasLastEvent{false};
        };

        bool buildCapturePlan(const ExperimentPlan& requestedPlan,
                              const QStringList& activeCameraIds,
                              ExperimentPlan& plan,
                              QString& errorMessage) const;
        bool planUsesMda(const ExperimentPlan& plan) const;
        bool planStreamsMda(const ExperimentPlan& plan) const;
        void resetCaptureState(const ExperimentPlan& plan);
        void resetSessionState(const ExperimentPlan& plan, const QJsonObject& deviceProperties);
        void finalizeActiveSession(ExperimentRunState state, const QString& errorMessage);
        void finishRecording(ExperimentRunState state, const QString& errorMessage = QString());
        static bool writeSessionDocument(const std::shared_ptr<RecordingSessionData>& session,
                                         QString& errorMessage);
        void appendPreviewEventRecord(const ImageFrame& frame);
        void primeLastFrameIndices();
        void emitProgress();
        bool startStreamingOutputs(const ExperimentPlan& plan);
        void stopStreamingOutputs(bool applyOutputManifest = false);
        void requestWriterStop();
        void writerLoop(const std::shared_ptr<CameraOutput>& output, quint64 generation);
        bool writeTask(CameraOutput& output, const WriteTask& task, QString& errorMessage);
        QString formatName(RecordingFormat format) const;
        void emitBufferUsageChanged(qint64 pendingWriteBytes);
        void emitWriterStatus();
        void setWriterStatus(RecordingWriterPhase phase, const QString& errorMessage = QString());
        QString writerErrorSnapshot() const;
        void setWriterError(const QString& errorMessage);
        static QString updateSessionResult(const std::shared_ptr<RecordingSessionData>& session,
                                           const QString& result,
                                           bool saved);
        bool enqueueFrame(const ImageFrame& frame);
        bool shouldAcceptFrame(const FramePacket& packet) const;

        void ingestFrame(const FramePacket& packet);
        void handleMdaOutput(const MDAOutput& output);
        bool startMdaCapture(QString* errorMessage = nullptr);
        bool startMdaRun(QString* errorMessage = nullptr);

        bool shouldCaptureCamera(const QString& cameraId) const;
        bool allCamerasReachedTarget() const;
        bool advanceBurstStateIfNeeded();

        MultiProcessCameraManager* m_mpcm{nullptr};
        std::shared_ptr<CMMCore> m_mmcore;
        std::function<bool(const QString&, ImageFrame&)> m_latestFrameFetcher;

        SessionState m_sessionState;
        WriterState m_writerState;
        CaptureState m_captureState;
        MdaState m_mdaState;
    };
}
