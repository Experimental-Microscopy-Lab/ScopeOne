#pragma once

#include "scopeone/ScopeOneCore.h"
#include "internal/MDAManager.h"
#include <QElapsedTimer>
#include <QHash>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

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

    class CameraManager;

    class RecordingManager : public QObject
    {
        Q_OBJECT

    public:
        explicit RecordingManager(QObject* parent = nullptr);
        ~RecordingManager() override;

        void setCameraManager(CameraManager* cameraManager) { m_cameraManager = cameraManager; }
        void setMMCore(const std::shared_ptr<CMMCore>& core) { m_mmcore = core; }

        void setLatestFrameFetcher(std::function<bool(const QString&, ImageFrame&)> fetcher)
        {
            m_latestFrameFetcher = std::move(fetcher);
        }

        void setSessionPreparationCallback(std::function<void(RecordingSessionData&)> callback)
        {
            m_sessionPreparationCallback = std::move(callback);
        }

        bool start(const ExperimentPlan& requestedPlan,
                   const QStringList& activeCameraIds,
                   const QJsonObject& deviceProperties,
                   const QHash<QString, double>& cameraPixelSizesUm);
        void stop();
        void shutdown();
        void setRecordedMaxBytes(qint64 bytes);
        qint64 recordedMaxBytes() const;

        bool isRecording() const { return m_captureState.isRecording; }
        bool isFinalizing() const { return m_completionPending; }

        void onRawFramesReady(const QList<ImageFrame>& frames);
        void onFrameDeliveryFailed(const QString& errorMessage, quint64 droppedFrames);

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
            AcquisitionEvent event;
            bool hasEvent{false};
        };

        struct WriteTask;
        struct CameraOutput;

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
            RecordingFormat format{RecordingFormat::OmeTiff};
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
        void resetSessionState(const ExperimentPlan& plan,
                               const QJsonObject& deviceProperties,
                               const QHash<QString, double>& cameraPixelSizesUm);
        void finalizeActiveSession(ExperimentRunState state, const QString& errorMessage);
        void finishRecording(ExperimentRunState state, const QString& errorMessage = QString());
        void completeRecording(ExperimentRunState state,
                               const QString& errorMessage,
                               const std::shared_ptr<RecordingSessionData>& session);
        void publishRecordingStopped(const std::shared_ptr<RecordingSessionData>& session);
        static bool writeSessionDocument(const RecordingSessionData& session,
                                         QString& errorMessage);
        void appendPreviewEventRecord(const ImageFrame& frame,
                                      const AcquisitionEvent& event);
        void primeLastFrameIndices();
        void emitProgress(bool force = false);
        bool startStreamingOutputs(const ExperimentPlan& plan);
        void stopStreamingOutputs(bool applyOutputManifest = false);
        void checkWriterFinalization();
        QList<std::shared_ptr<CameraOutput>> writerOutputsSnapshot() const;
        void requestWriterStop();
        void writerLoop(const std::shared_ptr<CameraOutput>& output, quint64 generation);
        bool writeTask(CameraOutput& output, const WriteTask& task, QString& errorMessage);
        void markWriterStatusDirty();
        void emitWriterStatus();
        void setWriterStatus(RecordingWriterPhase phase, const QString& errorMessage = QString());
        QString writerErrorSnapshot() const;
        void setWriterError(const QString& errorMessage);
        static QString updateSessionResult(RecordingSessionData& session,
                                           const QString& result,
                                           bool saved);
        bool enqueueFrame(const ImageFrame& frame,
                          const AcquisitionEvent* event = nullptr);
        bool shouldAcceptFrame(const FramePacket& packet) const;

        void ingestFrame(const FramePacket& packet);
        void handleMdaOutput(const MDAOutput& output);
        bool startMdaCapture(QString* errorMessage = nullptr);
        bool startMdaRun(QString* errorMessage = nullptr);

        bool allCamerasReachedTarget() const;
        void advanceBurstStateIfNeeded();

        CameraManager* m_cameraManager{nullptr};
        std::shared_ptr<CMMCore> m_mmcore;
        std::function<bool(const QString&, ImageFrame&)> m_latestFrameFetcher;
        std::function<void(RecordingSessionData&)> m_sessionPreparationCallback;

        std::shared_ptr<RecordingSessionData> m_activeSession;
        WriterState m_writerState;
        CaptureState m_captureState;
        MdaState m_mdaState;
        QElapsedTimer m_progressPublishTimer;
        QTimer m_writerStatusTimer;
        std::atomic_bool m_writerStatusDirty{false};
        bool m_completionPending{false};
        std::function<void()> m_writerFinalizationCompletion;
        QStringList m_incompleteOutputPaths;
        QThreadPool m_finalizationThreadPool;
    };
}
