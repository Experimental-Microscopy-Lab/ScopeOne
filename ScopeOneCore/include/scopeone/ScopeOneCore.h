#pragma once

#include <QObject>
#include <QStringList>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QVariantMap>
#include <QVector>
#include <memory>
#include <utility>
#include <vector>

#include "scopeone/ExperimentDocument.h"
#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

class CMMCore;
class QThreadPool;
class QTimer;

namespace scopeone::core
{
    class ImageSceneModel;

    namespace internal
    {
        class RecordingManager;
        class StageMosaicManager;
    }

    constexpr int kRecordingPhaseIdle = 0;
    constexpr int kRecordingPhaseRecording = 1;
    constexpr int kRecordingPhaseRecordingBurst = 2;
    constexpr int kRecordingPhaseRecordingMda = 3;
    constexpr int kRecordingPhaseWaitingNextBurst = 4;
    constexpr int kRecordingPhaseStopped = 5;

    class SCOPEONE_CORE_EXPORT ScopeOneCore : public QObject
    {
        Q_OBJECT

    public:
        using RecordingAxis = scopeone::core::RecordingAxis;
        using ProcessingModuleKind = scopeone::core::ProcessingModuleKind;
        using ProcessingBitDepth = scopeone::core::ProcessingBitDepth;
        using RecordingFileManifest = scopeone::core::RecordingFileManifest;
        using RecordingOutputManifest = scopeone::core::RecordingOutputManifest;

        struct RecordingSaveOptions
        {
            RecordingFormat format{RecordingFormat::Tiff};
            bool enableCompression{false};
            int compressionLevel{6};
            QString saveDir;
            QString baseName;
        };

        struct LoadConfigResult
        {
            QStringList cameraIds;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
        };

        struct HistogramStats
        {
            double mean{0.0};
            double minVal{0.0};
            double maxVal{0.0};
            double stdDev{0.0};
            std::vector<int> histogram;
            int totalPixels{0};
            int bitDepth{8};
            int maxValue{255};
            int autoMinLevel{0};
            int autoMaxLevel{255};

            bool hasData() const { return totalPixels > 0; }
        };

        struct ParticleMeasurement
        {
            int area{0};
            QRect bounds;
            QPointF centroid;
        };

        struct ParticleDetectionResult
        {
            QList<ParticleMeasurement> particles;
            ImageFrame mask;
            bool truncated{false};
        };

        struct StageMosaicPlan
        {
            QString cameraId;
            QString xyStageId;
            int rows{1};
            int columns{1};
            double pixelSizeUm{1.0};
            double stepXUm{0.0};
            double stepYUm{0.0};
            int settleMs{150};
            bool returnToStart{true};
            QString gallerySaveDir;
        };

        enum class StageMosaicState
        {
            Idle,
            Running,
            Completed,
            Canceled,
            Failed
        };

        struct StageMosaicStatus
        {
            StageMosaicState state{StageMosaicState::Idle};
            int completedTiles{0};
            int totalTiles{0};
            QString message;
            QString sessionId;
        };

        struct RecordingProgress
        {
            int phase{kRecordingPhaseIdle};
            qint64 frameCurrent{0};
            qint64 frameTarget{0};
            int burstCurrent{0};
            int burstTarget{0};
            qint64 waitRemainingMs{0};
            int timeIndex{0};
            int timeCount{0};
            int zIndex{0};
            int zCount{0};
            int positionIndex{0};
            int positionCount{0};
            bool hasXY{false};
            double x{0.0};
            double y{0.0};
            bool hasZ{false};
            double z{0.0};
        };

        class RecordingSaveResult
        {
        public:
            bool saved() const { return m_saved; }
            const QString& message() const { return m_message; }

            void reset()
            {
                m_saved = false;
                m_message.clear();
            }

            void set(bool saved, const QString& message)
            {
                m_saved = saved;
                m_message = message;
            }

        private:
            bool m_saved{false};
            QString m_message;
        };

        enum class RecordingWriterPhase
        {
            Idle,
            Starting,
            Writing,
            Stopping,
            Completed,
            Failed
        };

        class RecordingWriterStatus
        {
        public:
            RecordingWriterPhase phase() const { return m_phase; }
            qint64 pendingWriteBytes() const { return m_pendingWriteBytes; }
            qint64 maxPendingWriteBytes() const { return m_maxPendingWriteBytes; }
            qint64 framesWritten() const { return m_framesWritten; }
            const QString& errorMessage() const { return m_errorMessage; }

            bool isTerminal() const
            {
                return m_phase == RecordingWriterPhase::Completed
                    || m_phase == RecordingWriterPhase::Failed;
            }

            void reset(qint64 maxPendingWriteBytes = 0)
            {
                m_phase = RecordingWriterPhase::Idle;
                m_pendingWriteBytes = 0;
                m_maxPendingWriteBytes = maxPendingWriteBytes;
                m_framesWritten = 0;
                m_errorMessage.clear();
            }

            void setPhase(RecordingWriterPhase phase, const QString& errorMessage = QString())
            {
                m_phase = phase;
                if (phase == RecordingWriterPhase::Failed)
                {
                    m_errorMessage = errorMessage;
                }
                else if (!errorMessage.isNull())
                {
                    m_errorMessage = errorMessage;
                }
                else if (errorMessage.isEmpty())
                {
                    m_errorMessage.clear();
                }
            }

            void setPendingWriteBytes(qint64 pendingWriteBytes)
            {
                m_pendingWriteBytes = pendingWriteBytes;
            }

            void setMaxPendingWriteBytes(qint64 maxPendingWriteBytes)
            {
                m_maxPendingWriteBytes = maxPendingWriteBytes;
            }

            void addWrittenFrames(qint64 framesWritten)
            {
                m_framesWritten += framesWritten;
            }

            void setFrom(const RecordingWriterStatus& other)
            {
                m_phase = other.m_phase;
                m_pendingWriteBytes = other.m_pendingWriteBytes;
                m_maxPendingWriteBytes = other.m_maxPendingWriteBytes;
                m_framesWritten = other.m_framesWritten;
                m_errorMessage = other.m_errorMessage;
            }

        private:
            RecordingWriterPhase m_phase{RecordingWriterPhase::Idle};
            qint64 m_pendingWriteBytes{0};
            qint64 m_maxPendingWriteBytes{0};
            qint64 m_framesWritten{0};
            QString m_errorMessage;
        };

        class SCOPEONE_CORE_EXPORT RecordingSessionData
        {
        public:
            const QStringList& cameraIds() const { return m_manifest.plan.cameraIds; }
            const ExperimentPlan& capturePlan() const { return m_manifest.plan; }
            const ExperimentDocument& experimentDocument() const { return m_manifest; }
            ExperimentRunState runState() const { return m_manifest.runState; }
            const QString& errorMessage() const { return m_manifest.errorMessage; }
            bool streamedToDisk() const { return m_manifest.output.streamedToDisk; }

            QStringList recordedCameraIds() const
            {
                QStringList ids;
                for (const QString& cameraId : m_manifest.plan.cameraIds)
                {
                    const QString trimmedCameraId = cameraId.trimmed();
                    if (!trimmedCameraId.isEmpty() && !ids.contains(trimmedCameraId))
                    {
                        ids.append(trimmedCameraId);
                    }
                }
                for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it)
                {
                    if (!it.value().empty() && !ids.contains(it.key()))
                    {
                        ids.append(it.key());
                    }
                }
                for (auto it = m_manifest.output.files.constBegin(); it != m_manifest.output.files.constEnd(); ++it)
                {
                    const QString trimmedCameraId = it.key().trimmed();
                    if (!trimmedCameraId.isEmpty() && !ids.contains(trimmedCameraId))
                    {
                        ids.append(trimmedCameraId);
                    }
                }
                return ids;
            }

            const QHash<QString, RecordingFileManifest>& outputFiles() const
            {
                return m_manifest.output.files;
            }

            qint64 outputFrameCount(const QString& cameraId) const
            {
                const auto it = m_manifest.output.files.constFind(cameraId.trimmed());
                return it == m_manifest.output.files.constEnd() ? 0 : it.value().framesWritten;
            }

            qint64 outputFrameCount() const
            {
                qint64 total = 0;
                for (auto it = m_manifest.output.files.constBegin(); it != m_manifest.output.files.constEnd(); ++it)
                {
                    total += it.value().framesWritten;
                }
                return total;
            }

            qint64 recordedFrameCount(const QString& cameraId) const
            {
                const int bufferedFrames = frameCount(cameraId);
                return bufferedFrames > 0 ? bufferedFrames : outputFrameCount(cameraId);
            }

            qint64 recordedFrameCount() const
            {
                qint64 total = 0;
                for (const QString& cameraId : recordedCameraIds())
                {
                    total += recordedFrameCount(cameraId);
                }
                return total;
            }

            bool hasRecordedOutput() const
            {
                return recordedFrameCount() > 0;
            }

            bool isSaved() const { return m_saveResult.saved(); }
            const QString& saveMessage() const { return m_saveResult.message(); }

        private:
            friend class ScopeOneCore;
            friend class internal::RecordingManager;

            void setStreamedToDisk(bool streamedToDisk) { m_manifest.output.streamedToDisk = streamedToDisk; }
            bool hasFrames(const QString& cameraId) const
            {
                const auto it = m_frames.constFind(cameraId.trimmed());
                return it != m_frames.constEnd() && !it.value().empty();
            }
            int frameCount() const
            {
                int total = 0;
                for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it)
                {
                    total += static_cast<int>(it.value().size());
                }
                return total;
            }
            int frameCount(const QString& cameraId) const
            {
                const auto it = m_frames.constFind(cameraId.trimmed());
                return it == m_frames.constEnd() ? 0 : static_cast<int>(it.value().size());
            }
            bool hasAnyFrames() const
            {
                for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it)
                {
                    if (!it.value().empty())
                    {
                        return true;
                    }
                }
                return false;
            }
            void resetSaveResult()
            {
                m_saveResult.reset();
            }
            void setSaveResult(bool saved, const QString& message)
            {
                m_saveResult.set(saved, message);
            }
            void resetWriterStatus(qint64 maxPendingWriteBytes = 0)
            {
                m_writerStatus.reset(maxPendingWriteBytes);
            }
            void setWriterPhase(RecordingWriterPhase phase, const QString& errorMessage = QString())
            {
                m_writerStatus.setPhase(phase, errorMessage);
            }
            void addWrittenFrames(qint64 framesWritten)
            {
                m_writerStatus.addWrittenFrames(framesWritten);
            }
            void setWriterStatusSnapshot(const RecordingWriterStatus& status)
            {
                m_writerStatus.setFrom(status);
            }
            void prepareForSave(bool streamedToDisk, qint64 maxPendingWriteBytes = 0)
            {
                setStreamedToDisk(streamedToDisk);
                clearOutputFiles();
                resetSaveResult();
                resetWriterStatus(maxPendingWriteBytes);
            }
            void setCapturePlan(const ExperimentPlan& plan)
            {
                m_manifest.plan = plan;
                QStringList cameraIds;
                for (const QString& cameraId : m_manifest.plan.cameraIds)
                {
                    const QString trimmedCameraId = cameraId.trimmed();
                    if (!trimmedCameraId.isEmpty() && !cameraIds.contains(trimmedCameraId))
                    {
                        cameraIds.append(trimmedCameraId);
                    }
                }
                m_manifest.plan.cameraIds = cameraIds;
            }
            void setSoftwareSnapshot(const SoftwareSnapshot& software) { m_manifest.software = software; }
            void setDeviceProperties(const QJsonObject& properties) { m_manifest.deviceProperties = properties; }
            void setRunState(ExperimentRunState state,
                             quint64 completedTimestampNs = 0,
                             const QString& errorMessage = QString())
            {
                m_manifest.runState = state;
                m_manifest.completedTimestampNs = completedTimestampNs;
                m_manifest.errorMessage = errorMessage;
            }
            void setStartedTimestampNs(quint64 timestampNs) { m_manifest.startedTimestampNs = timestampNs; }
            void appendEventRecord(const AcquisitionEventRecord& record) { m_manifest.events.append(record); }
            void setPresentationState(const QList<DocumentLayer>& layers,
                                      const QList<DocumentMarkup>& markups)
            {
                m_manifest.layers = layers;
                m_manifest.markups = markups;
            }
            ImageFrame imageFrameAt(const QString& cameraId, int index) const;
            QList<ImageFrame> firstImageFrames() const;
            bool appendImageFrame(const ImageFrame& frame)
            {
                const QString cameraId = frame.cameraId.trimmed();
                if (!frame.isValid() || cameraId.isEmpty())
                {
                    return false;
                }
                ImageFrame storedFrame(frame);
                storedFrame.cameraId = cameraId;
                m_frames[cameraId].push_back(std::move(storedFrame));
                if (!m_manifest.plan.cameraIds.contains(cameraId))
                {
                    m_manifest.plan.cameraIds.append(cameraId);
                }
                return true;
            }
            bool appendImageFrames(const QList<ImageFrame>& frames);
            ImageFrame firstImageFrame(const QString& cameraId) const
            {
                return imageFrameAt(cameraId, 0);
            }
            static std::shared_ptr<RecordingSessionData> fromImageFrames(
                const QList<ImageFrame>& frames,
                const ExperimentPlan& capturePlan);
            void clearFrames() { m_frames.clear(); }
            void clearOutputFiles() { m_manifest.clearOutput(); }
            RecordingFileManifest& ensureFileManifest(const QString& cameraId)
            {
                return m_manifest.ensureFile(cameraId.trimmed());
            }
            void setOutputFilePaths(const QString& cameraId, const QString& rawPath, const QString& frameInfoPath)
            {
                auto& fileManifest = ensureFileManifest(cameraId);
                fileManifest.rawPath = rawPath;
                fileManifest.frameInfoPath = frameInfoPath;
            }
            void setOutputFramesWritten(const QString& cameraId, qint64 framesWritten)
            {
                ensureFileManifest(cameraId).framesWritten = framesWritten;
            }
            std::shared_ptr<RecordingSessionData> cloneForSave() const
            {
                auto clone = std::make_shared<RecordingSessionData>();
                clone->m_manifest = m_manifest;
                clone->m_frames = m_frames;
                clone->m_saveResult = m_saveResult;
                clone->m_writerStatus = m_writerStatus;
                return clone;
            }
            void applySaveStateFrom(const RecordingSessionData& source)
            {
                m_saveResult = source.m_saveResult;
                m_writerStatus = source.m_writerStatus;
                if (source.m_saveResult.saved())
                {
                    m_manifest.plan = source.m_manifest.plan;
                    m_manifest.output = source.m_manifest.output;
                }
            }

            ImageFrame outputImageFrameAt(const QString& cameraId, int index) const;
            const ImageFrame* frameAt(const QString& cameraId, int index) const;

            ExperimentDocument m_manifest;
            QHash<QString, std::vector<ImageFrame>> m_frames;
            RecordingSaveResult m_saveResult;
            RecordingWriterStatus m_writerStatus;
        };

        class ProcessingModuleInfo
        {
        public:
            ProcessingModuleKind kind() const { return m_kind; }
            const QString& name() const { return m_name; }
            const QVariantMap& parameters() const { return m_parameters; }
            void setKind(ProcessingModuleKind kind) { m_kind = kind; }
            void setName(const QString& name) { m_name = name; }
            void setParameters(const QVariantMap& parameters) { m_parameters = parameters; }

        private:
            ProcessingModuleKind m_kind{ProcessingModuleKind::Unknown};
            QString m_name;
            QVariantMap m_parameters;
        };

        class DevicePropertyInfo
        {
        public:
            const QString& name() const { return m_name; }
            const QString& value() const { return m_value; }
            const QString& type() const { return m_type; }
            bool isReadOnly() const { return m_readOnly; }
            bool isPreInit() const { return m_preInit; }
            const QStringList& allowedValues() const { return m_allowedValues; }
            bool hasLimits() const { return m_hasLimits; }
            double lowerLimit() const { return m_lowerLimit; }
            double upperLimit() const { return m_upperLimit; }

            void setName(const QString& name) { m_name = name; }
            void setValue(const QString& value) { m_value = value; }
            void setType(const QString& type) { m_type = type; }
            void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
            void setPreInit(bool preInit) { m_preInit = preInit; }
            void setAllowedValues(const QStringList& allowedValues) { m_allowedValues = allowedValues; }

            void setLimits(double lowerLimit, double upperLimit)
            {
                m_hasLimits = true;
                m_lowerLimit = lowerLimit;
                m_upperLimit = upperLimit;
            }

        private:
            QString m_name;
            QString m_value;
            QString m_type{QStringLiteral("Unknown")};
            bool m_readOnly{true};
            bool m_preInit{false};
            QStringList m_allowedValues;
            bool m_hasLimits{false};
            double m_lowerLimit{0.0};
            double m_upperLimit{0.0};
        };

        explicit ScopeOneCore(QObject* parent = nullptr);
        ~ScopeOneCore() override;
        static QString getVersion();
        static QString getMMCoreVersion();
        static QString getLibTiffVersion();
        static QString getZlibVersion();
        static QString rawLayerKey(const QString& cameraId);
        static QString processedLayerKey(const QString& cameraId);
        static QString staticLayerKey(const QString& sourceId);
        static QString sourceIdFromLayerKey(const QString& layerKey);
        static bool isRawLayerKey(const QString& layerKey);
        static bool isProcessedLayerKey(const QString& layerKey);
        static bool isStaticLayerKey(const QString& layerKey);
        ImageSceneModel* imageSceneModel() const { return m_imageSceneModel; }

        bool loadConfiguration(const QString& configPath,
                               LoadConfigResult* result,
                               QString* errorMessage);
        void unloadConfiguration();
        QString loadedConfigurationPath() const { return m_loadedConfigPath; }
        QString loadedConfigurationSha256() const { return m_loadedConfigSha256; }

        QStringList cameraIds() const { return m_cameraIds; }
        QStringList runningPreviewCameraIds() const;

        bool startPreview(const QString& cameraIdOrAll);
        bool stopPreview(const QString& cameraIdOrAll);
        bool setExposure(const QString& cameraIdOrAll, double exposureMs);
        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool setHalfROI(const QString& cameraId);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height);
        ImageFrame graphFrame(const QString& layerKey) const;
        QList<ImageFrame> graphFrames(const QStringList& layerKeys) const;
        bool graphPixelValue(const QString& layerKey, const QPoint& imagePos, int& value) const;
        ImageFrame sessionFrameAt(
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& cameraId,
            int index);
        QList<ImageFrame> firstSessionFrames(
            const std::shared_ptr<RecordingSessionData>& session);
        void removeSessionFrameSource(const std::shared_ptr<RecordingSessionData>& session);
        std::shared_ptr<RecordingSessionData> createFrameSession(
            const QList<ImageFrame>& frames,
            const ExperimentPlan& capturePlan);
        ImageFrame publishStaticFrame(const QString& sourceId,
                                      const ImageFrame& frame,
                                      const QString& displayName = QString());
        ImageFrame publishExternalFrame(const QString& sourceId, const ImageFrame& frame);
        void removeStaticFrame(const QString& sourceId);
        void clearStaticFrames();
        void clearLiveFrames(const QString& cameraId);
        void clearProcessedFrames();
        bool getRawImageStatistics(const QString& cameraId, HistogramStats& stats) const;
        bool getLayerHistogram(const QString& layerKey, HistogramStats& stats) const;
        void setActiveHistogramLayer(const QString& layerKey);
        bool autoLayerLevels(const QString& layerKey);
        bool fullLayerLevels(const QString& layerKey);
        bool setLayerAutoStretchEnabled(const QString& layerKey, bool enabled);
        bool layerAutoStretchEnabled(const QString& layerKey) const;
        bool getLineProfile(const QString& layerKey,
                            const QPoint& start,
                            const QPoint& end,
                            QVector<int>& values) const;
        bool detectParticles(const QString& layerKey,
                             int threshold,
                             int minArea,
                             int maxArea,
                             ParticleDetectionResult& result,
                             int maxParticles = 10000) const;
        static bool computeHistogramStats(const ImageFrame& frame, HistogramStats& stats);

        bool startStageMosaic(const StageMosaicPlan& plan,
                              QString* errorMessage = nullptr);
        void cancelStageMosaic();
        bool isStageMosaicRunning() const;
        StageMosaicStatus stageMosaicStatus() const;

        QStringList xyStageDevices() const;
        QStringList zStageDevices() const;
        QString currentXYStageDevice() const;
        QString currentFocusDevice() const;
        bool readXYPosition(const QString& xyStageLabel, double& x, double& y) const;
        bool readZPosition(const QString& zStageLabel, double& z) const;
        bool moveXYRelative(const QString& xyStageLabel, double dx, double dy);
        bool moveZRelative(const QString& zStageLabel, double dz);
        bool moveXYTo(const QString& xyStageLabel, double x, double y);
        bool moveZTo(const QString& zStageLabel, double z);
        bool readExposure(const QString& cameraIdOrAll, double& exposureMs) const;

        QStringList availableConfigGroups() const;
        QStringList availableConfigs(const QString& configGroup) const;
        QString currentConfig(const QString& groupName) const;
        bool setConfig(const QString& groupName, const QString& configName);


        QStringList loadedDevices() const;
        QList<DevicePropertyInfo> deviceProperties(const QString& deviceLabel, bool fromCache) const;
        QStringList devicePropertyNames(const QString& deviceLabel) const;
        QString getPropertyValue(const QString& deviceLabel, const QString& name, bool fromCache) const;
        QString propertyTypeString(const QString& deviceLabel, const QString& name) const;
        bool isPropertyReadOnly(const QString& deviceLabel, const QString& name) const;
        QStringList getAllowedPropertyValues(const QString& deviceLabel, const QString& name) const;
        bool getPropertyLimits(const QString& deviceLabel,
                               const QString& name,
                               double& lower,
                               double& upper) const;
        bool setPropertyValue(const QString& deviceLabel,
                              const QString& name,
                              const QString& value,
                              QString* errorMessage = nullptr);


        bool isRealTimeProcessingEnabled() const;
        bool setRealTimeProcessingEnabled(bool enabled);
        ProcessingBitDepth processingBitDepth() const;
        bool setProcessingBitDepth(ProcessingBitDepth bitDepth);
        ProcessingRecipe processingRecipe() const;
        bool applyProcessingRecipe(const ProcessingRecipe& recipe, QString* errorMessage = nullptr);
        ImageFrame processFrame(const ImageFrame& frame) const;
        ImageFrame processFrameFrom(int startModuleIndex, const ImageFrame& frame) const;
        ImageFrame processFrameThrough(int endModuleIndex, const ImageFrame& frame) const;
        QList<ProcessingModuleInfo> processingModules() const;
        bool addProcessingModule(ProcessingModuleKind kind);
        bool removeProcessingModule(int index);
        bool setProcessingModuleParameters(int index, const QVariantMap& parameters);
        bool resetProcessingModuleState(int index);


        void setRecordingMaxPendingWriteBytes(qint64 bytes);
        qint64 recordingMaxPendingWriteBytes() const;
        RecordingProgress recordingProgress() const;
        RecordingWriterStatus recordingWriterStatus() const;
        bool startRecording(const ExperimentPlan& plan, const QStringList& activeCameraIds);
        void stopRecording();
        bool isRecording() const;
        bool startExperiment(const ExperimentDocument& document,
                             QString* errorMessage = nullptr);
        bool cancelExperiment(const QString& experimentId,
                              QString* errorMessage = nullptr);
        QString activeExperimentId() const;
        bool experimentCancelRequested() const;
        QStringList experimentIds() const;
        bool experimentDocument(const QString& experimentId,
                                ExperimentDocument& document) const;
        QStringList recordingSessionIds() const;
        std::shared_ptr<RecordingSessionData> recordingSession(const QString& sessionId) const;
        bool closeRecordingSession(const QString& sessionId);
        bool setRecordingSessionPresentation(
            const std::shared_ptr<RecordingSessionData>& session,
            const ExperimentDocument& presentation,
            QString* errorMessage = nullptr);
        QString saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session);
        QString saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session,
                                     const RecordingSaveOptions& saveOptions);
        void saveRecordingSessionAsync(const std::shared_ptr<RecordingSessionData>& session);

    signals:
        void hardwareConfigurationChanged();
        void deviceStateChanged();
        void stagePositionChanged();
        void newRawFrameReady(const ImageFrame& frame);
        void rawFramesAcquired(const QString& cameraId, quint64 frameCount);
        void previewRawFrameReady(const ImageFrame& frame);
        void previewStateChanged(bool running);
        void agentControlServerListening(const QString& cameraId, const QString& serverName);
        void processedFrameReady(const ImageFrame& frame);
        void processedFramesCompleted(const QString& cameraId, quint64 frameCount);
        void previewProcessedFrameReady(const ImageFrame& frame);
        void staticFramePublished(const QString& sourceId,
                                  const QString& displayName,
                                  const ImageFrame& frame);
        void staticFrameRemoved(const QString& sourceId);
        void staticFramesCleared();
        void liveFramesCleared(const QString& cameraId);
        void processedFramesCleared();
        void imageHistogramReady(const QString& cameraId, bool processed, const HistogramStats& stats);
        void layerHistogramReady(const QString& layerKey, const HistogramStats& stats);
        void layerAnalysisCleared(const QString& layerKey);
        void lineProfileUpdated(const QString& cameraId, bool processed, const QVector<int>& values);
        void layerLineProfileUpdated(const QString& layerKey, const QVector<int>& values);
        void lineProfileCleared();
        void stageMosaicProgress(int completedTiles, int totalTiles, const QString& message);
        void stageMosaicFrameUpdated(const ImageFrame& frame);
        void stageMosaicFinished(
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& message,
            bool canceled);
        void processingError(const QString& errorMessage);
        void processingModulesChanged();
        void processingModuleParametersChanged(int index);
        void processingSettingsChanged();

        void recordingProgressChanged(int phase,
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
        void recordingWriterStatusChanged(const RecordingWriterStatus& status);
        void recordingStateChanged(bool isRecording);
        void recordingStopped(const std::shared_ptr<RecordingSessionData>& session);
        void recordingSessionSaveFinished(const std::shared_ptr<RecordingSessionData>& session);
        void recordingSessionClosed(const QString& sessionId);

    private:
        struct Managers;

        enum class FrameGraphStream
        {
            Raw,
            Processed,
            Static,
            External
        };

        class FrameGraph
        {
        public:
            void clear();
            bool publishLatest(FrameGraphStream stream, const ImageFrame& frame);
            bool publishLatest(FrameGraphStream stream, const QString& sourceId, const ImageFrame& frame);
            ImageFrame latest(FrameGraphStream stream, const QString& sourceId) const;
            bool publishSessionSource(const QString& sourceId,
                                      const std::shared_ptr<RecordingSessionData>& session,
                                      const QList<ImageFrame>& firstFrames);
            std::shared_ptr<RecordingSessionData> sessionSource(const QString& sourceId) const;
            QList<ImageFrame> sessionFirstFrames(const QString& sourceId) const;
            void removeSessionSource(const QString& sourceId);
            void remove(FrameGraphStream stream, const QString& sourceId);
            void clear(FrameGraphStream stream);

        private:
            struct SessionSource
            {
                std::weak_ptr<RecordingSessionData> session;
                QList<ImageFrame> firstFrames;
            };

            QHash<QString, ImageFrame>& latestMap(FrameGraphStream stream);
            const QHash<QString, ImageFrame>& latestMap(FrameGraphStream stream) const;

            QHash<QString, ImageFrame> m_rawFrames;
            QHash<QString, ImageFrame> m_processedFrames;
            QHash<QString, ImageFrame> m_staticFrames;
            QHash<QString, ImageFrame> m_externalFrames;
            QHash<QString, SessionSource> m_sessionSources;
        };

        struct HistogramJobState
        {
            bool inFlight{false};
            bool retryScheduled{false};
            qint64 lastScheduledMs{0};
            quint64 activeSequence{0};
            ImageFrame queuedFrame;
        };

        bool loadConfigurationInternal(const QString& configPath,
                                       LoadConfigResult* result,
                                       QString* errorMessage);
        std::shared_ptr<CMMCore> core() const;
        bool isConfiguredCamera(const QString& deviceLabel) const;
        bool isNativeCamera(const QString& deviceLabel) const;
        bool isPropertyPreInit(const QString& deviceLabel, const QString& name) const;
        void ensureSceneLayer(const QString& layerKey,
                              const QString& sourceId,
                              const QString& name,
                              DocumentLayerKind kind);
        void handleIncomingRawFrame(const ImageFrame& frame);
        void processGraphRawFrameAsync(const ImageFrame& frame);
        void queuePreviewRawFrame(const ImageFrame& frame);
        void queuePreviewProcessedFrame(const ImageFrame& frame);
        void schedulePreviewFlush();
        void flushPreviewFrames();
        QString sessionFrameSourceId(const std::shared_ptr<RecordingSessionData>& session) const;
        bool publishSessionFrameSource(const std::shared_ptr<RecordingSessionData>& session);
        bool histogramUpdatesEnabled(const QString& layerKey) const;
        void scheduleHistogramStats(const QString& layerKey, const ImageFrame& frame);
        void clearLayerAnalysis(const QString& layerKey);
        void clearLayerAnalysisByPrefix(const QString& prefix);
        void updateLineProfile(const QString& cameraId,
                               bool processed,
                               const ImageFrame& frame);
        bool updateStaticLineProfile(const QString& sourceId, const ImageFrame& frame);
        void setLineProfile(const QString& cameraId,
                            const QPoint& start,
                            const QPoint& end,
                            bool processed);
        void setStaticLineProfile(const QString& sourceId,
                                  const QPoint& start,
                                  const QPoint& end);
        void clearLineProfile();
        void syncLineProfileFromScene();
        void registerRecordingSession(const std::shared_ptr<RecordingSessionData>& session);
        void finalizeActiveExperiment(const std::shared_ptr<RecordingSessionData>& session);

        struct ActiveLineProfile
        {
            QString sourceId;
            QPoint start;
            QPoint end;
            bool processed{false};
            bool staticSource{false};
            bool active{false};
        };

        std::unique_ptr<Managers> m_managers;
        ImageSceneModel* m_imageSceneModel{nullptr};
        QStringList m_cameraIds;
        QString m_loadedConfigPath;
        QString m_loadedConfigSha256;
        ActiveLineProfile m_activeLineProfile;
        FrameGraph m_frameGraph;
        QHash<QString, ImageFrame> m_pendingPreviewRawFrames;
        QHash<QString, ImageFrame> m_pendingPreviewProcessedFrames;
        QHash<QString, HistogramJobState> m_histogramJobStates;
        QHash<QString, HistogramStats> m_latestHistogramStats;
        std::unique_ptr<QThreadPool> m_histogramThreadPool;
        QString m_activeHistogramLayerKey;
        quint64 m_nextHistogramSequence{0};
        QElapsedTimer m_lineProfileUpdateTimer;
        QElapsedTimer m_previewPublishTimer;
        QTimer* m_previewFlushTimer{nullptr};
        QSet<const RecordingSessionData*> m_sessionsSaving;
    };
}

Q_DECLARE_METATYPE(std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::RecordingWriterStatus)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::HistogramStats)
