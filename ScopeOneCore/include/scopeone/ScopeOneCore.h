#pragma once

#include <QObject>
#include <QStringList>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QPoint>
#include <QPointF>
#include <QVariantMap>
#include <QVector>
#include <memory>
#include <utility>
#include <vector>

#include "scopeone/ImageFrame.h"
#include "scopeone/SharedFrame.h"
#include "scopeone/scopeone_core_export.h"

class CMMCore;

namespace scopeone::core
{
    enum class RecordingFormat
    {
        Tiff = 0,
        Binary = 1
    };

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
        enum class RecordingAxis
        {
            Time = 0,
            Z = 1,
            XY = 2
        };

        enum class ProcessingModuleKind
        {
            FFT = 0,
            MedianFilter = 1,
            BackgroundCalibration = 2,
            SpatiotemporalBinning = 3,
            GaussianBlur = 4,
            Unknown = 255
        };

        struct LoadConfigResult
        {
            QStringList cameraIds;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
        };

        struct RecordingSettings
        {
            RecordingFormat format{RecordingFormat::Tiff};
            bool enableCompression{false};
            int compressionLevel{6};
            int framesPerBurst{1};
            bool burstMode{false};
            int targetBursts{1};
            double burstIntervalMs{0.0};
            double mdaIntervalMs{0.0};
            std::vector<QPointF> positions;
            std::vector<double> zPositions;
            std::vector<RecordingAxis> order{RecordingAxis::Time, RecordingAxis::Z, RecordingAxis::XY};
            QString saveDir;
            QString baseName;
            bool captureAll{true};
            QString metadataFileName;
            QByteArray sessionMetadataJson;
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

        struct RecordingFrame
        {
            SharedFrameHeader header{};
            QByteArray rawData;
            int width{0};
            int height{0};
            int bits{0};
        };

        struct RecordingFileManifest
        {
            QString rawPath;
            QString frameInfoPath;
            qint64 framesWritten{0};
        };

        struct RecordingCapturePlanData
        {
            QStringList cameraIds;
            RecordingFormat format{RecordingFormat::Tiff};
            bool captureAll{true};
            bool enableCompression{false};
            int compressionLevel{6};
            int framesPerBurst{1};
            bool burstMode{false};
            int targetBursts{1};
            double burstIntervalMs{0.0};
            double mdaIntervalMs{0.0};
            std::vector<RecordingAxis> order{RecordingAxis::Time, RecordingAxis::Z, RecordingAxis::XY};
            std::vector<QPointF> positions;
            std::vector<double> zPositions;
            QString saveDir;
            QString baseName;
            QString metadataFileName;
            QByteArray sessionMetadataJson;
        };

        struct RecordingOutputManifest
        {
            bool streamedToDisk{false};
            QHash<QString, RecordingFileManifest> files;

            void clearFiles()
            {
                files.clear();
            }
        };

        struct RecordingManifest
        {
            RecordingCapturePlanData plan;
            RecordingOutputManifest output;

            const QStringList& cameraIds() const
            {
                return plan.cameraIds;
            }

            RecordingFileManifest& ensureFile(const QString& cameraId)
            {
                return output.files[cameraId];
            }

            void clearOutput()
            {
                output.clearFiles();
            }
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

        class RecordingSessionData
        {
        public:
            void setCapturePlan(const RecordingCapturePlanData& planData) { m_manifest.plan = planData; }
            const QStringList& cameraIds() const { return m_manifest.plan.cameraIds; }
            const RecordingCapturePlanData& capturePlan() const { return m_manifest.plan; }
            void setStreamedToDisk(bool streamedToDisk) { m_manifest.output.streamedToDisk = streamedToDisk; }
            bool streamedToDisk() const { return m_manifest.output.streamedToDisk; }

            bool hasFrames(const QString& cameraId) const
            {
                const auto it = m_frames.constFind(cameraId);
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

            QStringList recordedCameraIds() const
            {
                QStringList ids = m_manifest.plan.cameraIds;
                for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it)
                {
                    if (!it.value().empty() && !ids.contains(it.key()))
                    {
                        ids.append(it.key());
                    }
                }
                return ids;
            }

            const std::vector<RecordingFrame>* framesForCamera(const QString& cameraId) const
            {
                const auto it = m_frames.constFind(cameraId);
                return it == m_frames.constEnd() ? nullptr : &it.value();
            }

            std::vector<RecordingFrame>& ensureFramesForCamera(const QString& cameraId)
            {
                return m_frames[cameraId];
            }

            void appendFrame(const QString& cameraId, RecordingFrame frame)
            {
                ensureFramesForCamera(cameraId).push_back(std::move(frame));
                if (!m_manifest.plan.cameraIds.contains(cameraId))
                {
                    m_manifest.plan.cameraIds.append(cameraId);
                }
            }

            void clearFrames() { m_frames.clear(); }
            void clearOutputFiles() { m_manifest.clearOutput(); }

            RecordingFileManifest& ensureFileManifest(const QString& cameraId)
            {
                return m_manifest.ensureFile(cameraId);
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

            void resetSaveResult()
            {
                m_saveResult.reset();
            }

            void setSaveResult(bool saved, const QString& message)
            {
                m_saveResult.set(saved, message);
            }

            bool isSaved() const { return m_saveResult.saved(); }
            const QString& saveMessage() const { return m_saveResult.message(); }

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

        private:
            RecordingManifest m_manifest;
            QHash<QString, std::vector<RecordingFrame>> m_frames;
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

        bool hasCore() const;


        bool loadConfiguration(const QString& configPath,
                               LoadConfigResult* result,
                               QString* errorMessage);
        void unloadConfiguration();

        QStringList cameraIds() const { return m_cameraIds; }

        void startPreview(const QString& cameraIdOrAll);
        void stopPreview(const QString& cameraIdOrAll);
        bool setExposure(const QString& cameraIdOrAll, double exposureMs);
        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool clearROI(const QString& cameraId);
        void setLineProfile(const QString& cameraId, const QPoint& start, const QPoint& end, bool processed);
        void clearLineProfile();
        bool getLatestRawFrame(const QString& cameraId, ImageFrame& frame) const;
        bool getRawImageStatistics(const QString& cameraId, HistogramStats& stats) const;
        static bool computeHistogramStats(const ImageFrame& frame, HistogramStats& stats);


        QStringList xyStageDevices() const;
        QStringList zStageDevices() const;
        QString currentXYStageDevice() const;
        QString currentFocusDevice() const;
        bool readXYPosition(const QString& xyStageLabel, double& x, double& y) const;
        bool readZPosition(const QString& zStageLabel, double& z) const;
        bool moveXYRelative(const QString& xyStageLabel, double dx, double dy);
        bool moveZRelative(const QString& zStageLabel, double dz);
        bool readExposure(const QString& cameraIdOrAll, double& exposureMs) const;


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
        void setRealTimeProcessingEnabled(bool enabled);
        void processFrameAsync(const ImageFrame& frame);
        QList<ProcessingModuleInfo> processingModules() const;
        bool addProcessingModule(ProcessingModuleKind kind);
        bool removeProcessingModule(int index);
        bool setProcessingModuleParameters(int index, const QVariantMap& parameters);
        bool resetProcessingModuleState(int index);


        void setRecordingAvailableCameras(const QStringList& cameraIds);
        void setRecordingMaxPendingWriteBytes(qint64 bytes);
        qint64 recordingMaxPendingWriteBytes() const;
        bool startRecording(const RecordingSettings& settings, const QStringList& activeCameraIds);
        void stopRecording();
        bool isRecording() const;
        QString saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session) const;
        void saveRecordingSessionAsync(const std::shared_ptr<RecordingSessionData>& session);

    signals:
        void newRawFrameReady(const ImageFrame& frame);
        void previewStateChanged(bool running);
        void agentControlServerListening(const QString& cameraId, const QString& serverName);
        void processedFrameReady(const QString& cameraId, const ImageFrame& frame);
        void imageHistogramReady(const QString& cameraId, bool processed, const HistogramStats& stats);
        void lineProfileUpdated(const QString& cameraId, bool processed, const QVector<int>& values);
        void lineProfileCleared();
        void processingError(const QString& errorMessage);
        void processingModulesChanged();

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

    private:
        struct Managers;

        struct HistogramJobState
        {
            bool inFlight{false};
            qint64 lastScheduledMs{0};
        };

        bool loadConfigurationInternal(const QString& configPath,
                                       const QStringList& existingCameraIds,
                                       LoadConfigResult* result,
                                       QString* errorMessage);
        std::shared_ptr<CMMCore> core() const;
        bool isAgentCamera(const QString& deviceLabel) const;
        bool isPropertyPreInit(const QString& deviceLabel, const QString& name) const;
        bool getLatestRawTransport(const QString& cameraId,
                                   SharedFrameHeader& header,
                                   QByteArray& data) const;
        void scheduleHistogramStats(const QString& cameraId,
                                    bool processed,
                                    const ImageFrame& frame);
        void updateLineProfile(const QString& cameraId,
                               bool processed,
                               const ImageFrame& frame);

        struct ActiveLineProfile
        {
            QString cameraId;
            QPoint start;
            QPoint end;
            bool processed{false};
            bool active{false};
        };

        std::unique_ptr<Managers> m_managers;
        QStringList m_cameraIds;
        ActiveLineProfile m_activeLineProfile;
        QHash<QString, ImageFrame> m_latestProcessedFrames;
        QHash<QString, HistogramJobState> m_histogramJobStates;
        QHash<QString, HistogramStats> m_latestHistogramStats;
    };
}

Q_DECLARE_METATYPE(std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::RecordingWriterStatus)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::HistogramStats)
