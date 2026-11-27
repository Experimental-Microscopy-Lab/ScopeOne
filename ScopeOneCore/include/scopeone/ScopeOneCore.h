#pragma once

#include <QObject>
#include <QStringList>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QMap>
#include <QMutex>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "scopeone/AcquisitionHook.h"
#include "scopeone/CameraSettings.h"
#include "scopeone/ExperimentDocument.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/HardwareTypes.h"
#include "scopeone/HardwareConfiguration.h"
#include "scopeone/Device.h"
#include "scopeone/ImageFrame.h"
#include "scopeone/ProcessingTypes.h"
#include "scopeone/ProcessingPipeline.h"
#include "scopeone/RecordingSession.h"
#include "scopeone/SignalSource.h"
#include "scopeone/scopeone_core_export.h"

class QThreadPool;
class QTimer;

namespace scopeone::core
{
    class ImageSceneModel;

    namespace internal
    {
        class ProcessingPipelineDefinition;
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
        using ProcessingBitDepth = scopeone::core::ProcessingBitDepth;
        using RecordingFileManifest = scopeone::core::RecordingFileManifest;
        using RecordingOutputManifest = scopeone::core::RecordingOutputManifest;

        struct RecordingSaveOptions
        {
            RecordingFormat format{RecordingFormat::OmeTiff};
            bool enableCompression{false};
            int compressionLevel{6};
            QString saveDir;
            QString baseName;
        };

        struct LoadConfigResult
        {
            QStringList cameraIds;
            QStringList failedDevices;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
            QList<HardwareDeviceDescriptor> devices;
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

        using RecordingWriterPhase = scopeone::core::RecordingWriterPhase;
        using RecordingWriterStatus = scopeone::core::RecordingWriterStatus;
        using RecordingSessionData = scopeone::core::RecordingSessionData;

        class ProcessingModuleInfo
        {
        public:
            const QString& id() const { return m_id; }
            const QString& name() const { return m_name; }
            const QVariantMap& parameters() const { return m_parameters; }
            const ProcessingModuleDescriptor& descriptor() const { return m_descriptor; }
            bool enabled() const { return m_enabled; }
            void setId(const QString& id) { m_id = id; }
            void setName(const QString& name) { m_name = name; }
            void setParameters(const QVariantMap& parameters) { m_parameters = parameters; }
            void setDescriptor(const ProcessingModuleDescriptor& descriptor) { m_descriptor = descriptor; }
            void setEnabled(bool enabled) { m_enabled = enabled; }

        private:
            QString m_id;
            QString m_name;
            QVariantMap m_parameters;
            ProcessingModuleDescriptor m_descriptor;
            bool m_enabled{true};
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

        // One light source channel of the ScopeOneLightSource lines of the cfg file. A role whose
        // property is empty is not declared, Power always is. Each role names its own device.
        struct LightSource
        {
            QString name;
            QString enableDevice;
            QString enableProperty;
            QString onValue;
            QString offValue;
            QString powerDevice;
            QString powerProperty;
            QString readingDevice;
            QString readingProperty;
        };

        // What one round over the light sources returns, in the order of the list that was passed.
        // The settings are empty when the round did not ask for them.
        struct LightSourceState
        {
            bool hasLimits{false};
            double lower{0.0};
            double upper{0.0};
            QString enable;
            QString power;
            QString reading;
        };

        // A rule that Core checks on every measurement of the live processing. When a measurement
        // of that name goes above or below the threshold, Core performs the action itself and then
        // waits cooldownMs before the rule can fire again.
        struct Trigger
        {
            enum class Compare
            {
                Above,
                Below
            };

            enum class Action
            {
                // Records frames from the camera that measured, as the Local API record request does
                Record,
                // Applies preset of group, through setConfig
                ApplyPreset,
                // Stops the preview of the camera that measured
                StopPreview
            };

            QString measurement;
            // Empty for any camera
            QString cameraId;
            Compare compare{Compare::Above};
            double threshold{0.0};
            Action action{Action::Record};
            int frames{100};
            // For record: frames the camera took up to the moment the trigger fires, which the
            // recording starts with. Core keeps that many of the newest frames of the camera while
            // the trigger is set. The frame that crossed the threshold is among them, as far back as
            // the live processing lags behind the camera.
            int preTriggerFrames{0};
            QString group;
            QString preset;
            double cooldownMs{5000.0};
        };

        using HookPoint = scopeone::core::HookPoint;
        using HookReply = scopeone::core::HookReply;

        // One change made to the microscope from outside the user interface: a Local API request
        // (origin api), a console command (console) or a trigger action (trigger)
        struct ActionLogEntry
        {
            QDateTime time;
            QString origin;
            QString operation;
            QJsonObject parameters;
            bool success{true};
            QString error;
        };

        explicit ScopeOneCore(QObject* parent = nullptr);
        ~ScopeOneCore() override;
        static QString getVersion();
        static QString getMMCoreVersion();
        static QString getOpenCVVersion();
        static QString getScopeWriterVersion();
        static QString getLibTiffVersion();
        static QString getZlibVersion();
        static QString rawLayerKey(const QString& cameraId);
        static QString processedLayerKey(const QString& cameraId);
        static QString toolLayerKey(const QString& sourceId);
        static QString staticLayerKey(const QString& sourceId);
        static QString sourceIdFromLayerKey(const QString& layerKey);
        static bool isRawLayerKey(const QString& layerKey);
        static bool isProcessedLayerKey(const QString& layerKey);
        static bool isToolLayerKey(const QString& layerKey);
        static bool isStaticLayerKey(const QString& layerKey);
        static QString triggerActionName(Trigger::Action action);
        static bool triggerActionFromName(const QString& name, Trigger::Action& action);
        static QString hookPointName(HookPoint point);
        static bool hookPointFromName(const QString& name, HookPoint& point);
        // time is ISO 8601 UTC with milliseconds
        static QJsonObject actionLogEntryToJson(const ActionLogEntry& entry);
        ImageSceneModel* imageSceneModel() const { return m_imageSceneModel; }

        bool loadConfiguration(const QString& configPath);
        bool unloadConfiguration();
        bool configurationOperationRunning() const { return m_configurationOperationRunning; }
        QString configurationState() const;
        QString configurationError() const { return m_configurationError; }
        QStringList configurationFailedDevices() const { return m_configurationFailedDevices; }
        QString loadedConfigurationPath() const { return m_loadedConfigPath; }
        QString loadedConfigurationSha256() const { return m_loadedConfigSha256; }
        QStringList additionalDeviceAdapterSearchPaths() const;
        bool setAdditionalDeviceAdapterSearchPaths(const QStringList& paths);

        QStringList cameraIds() const { return m_cameraIds; }
        QString configuredCamera() const;
        QList<HardwareDeviceDescriptor> hardwareDevices() const;
        QStringList runningPreviewCameraIds() const;
        double cameraPixelSizeUm(const QString& cameraId) const;
        bool setCameraPixelSizeUm(const QString& cameraId, double pixelSizeUm);

        bool startPreview(const QString& cameraIdOrAll);
        bool stopPreview(const QString& cameraIdOrAll, QString* errorMessage = nullptr);
        bool setExposure(const QString& cameraIdOrAll, double exposureMs, QString* errorMessage = nullptr);
        bool setROI(const QString& cameraId, int x, int y, int width, int height);
        bool setHalfROI(const QString& cameraId);
        bool clearROI(const QString& cameraId);
        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height);
        QList<SignalSourceDescriptor> signalSources() const;
        bool startSignalTrace(const SignalAcquisitionConfig& config,
                              QString* errorMessage = nullptr);
        void stopSignalTrace(const QString& sourceId);
        SignalSourceState signalSourceState(const QString& sourceId) const;
        QString signalSourceStateMessage(const QString& sourceId) const;
        QList<DaqDeviceDescriptor> daqDevices() const;
        bool startDaqSession(const DaqSessionConfig& config,
                             QString* errorMessage = nullptr);
        void stopDaqSession(const QString& deviceId);
        QString daqStateMessage(const QString& deviceId) const;
        ImageFrame graphFrame(const QString& layerKey) const;
        QList<ImageFrame> graphFrames(const QStringList& layerKeys) const;
        bool graphPixelValue(const QString& layerKey, const QPoint& imagePos, int& value) const;
        std::shared_ptr<RecordingSessionData> createFrameSession(
            const QList<ImageFrame>& frames,
            const ExperimentPlan& capturePlan);
        ImageFrame publishStaticFrame(const QString& sourceId,
                                      const ImageFrame& frame,
                                      const QString& displayName = QString());
        // One image of a file that holds several, such as the positions of an OME-TIFF
        struct ImageSeries
        {
            QString name;
            int firstPlane{0};
            int planeCount{0};
            StackShape shape;
        };
        // The images of a file, which is empty when the file holds one image
        static QList<ImageSeries> imageSeries(const QString& filePath);
        // Opens an image file. A negative series opens all of its planes as one image.
        void openImage(const QString& filePath, int series = -1);
        // Reads all the planes of an image file at once, for work that goes through many files
        static QList<ImageFrame> readImageFrames(const QString& filePath, QString* errorMessage = nullptr);
        // Writes planes as a multi page TIFF file
        static bool writeImageFrames(const QList<ImageFrame>& frames, const QString& filePath,
                                     QString* errorMessage = nullptr);
        int layerSliceCount(const QString& layerKey) const;
        void setLayerSliceIndex(const QString& layerKey, int sliceIndex);
        ImageFrame publishToolStreamFrame(const QString& sourceId,
                                          const ImageFrame& frame,
                                          const QString& displayName = QString());
        ImageFrame publishExternalFrame(const QString& sourceId, const ImageFrame& frame);
        void removeStaticFrame(const QString& sourceId);
        void clearStaticFrames();
        void clearLiveFrames(const QString& cameraId);
        void clearProcessedFrames();
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
        quint64 detectParticles(const QString& layerKey,
                                int threshold,
                                int minArea,
                                int maxArea,
                                int maxParticles = 10000);
        quint64 detectParticles(const ImageFrame& frame,
                                const QString& resultLayerKey,
                                int threshold,
                                int minArea,
                                int maxArea,
                                int maxParticles = 10000);
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
        quint64 moveXYRelative(const QString& xyStageLabel, double dx, double dy);
        quint64 moveZRelative(const QString& zStageLabel, double dz);
        quint64 moveXYTo(const QString& xyStageLabel, double x, double y);
        quint64 moveZTo(const QString& zStageLabel, double z);
        bool setShutterOpen(const QString& shutterLabel,
                            bool open,
                            QString* errorMessage = nullptr);
        bool readExposure(const QString& cameraIdOrAll, double& exposureMs) const;

        // The light source channels of the loaded devices in cfg order. Empty during a configuration load
        // or unload.
        QList<LightSource> lightSources() const;
        bool setLightSource(const LightSource& source, QString* errorMessage = nullptr);
        // Takes the roles, the light sources and the limits of the loaded cfg file again after it
        // was edited, without restarting a device
        bool reloadRoles(QString* errorMessage = nullptr);
        // The ScopeOneLimit ranges of the loaded cfg. Core refuses any command that would set a
        // limited property, exposure or stage position outside its range.
        QList<HardwareConfiguration::Limit> limits() const;
        // Refuses an acquisition plan that would leave a limit, before anything moves
        bool checkPlanLimits(const ExperimentPlan& plan, QString* errorMessage = nullptr) const;
        // Reads the measured power of the light sources on the hardware thread, which also runs the
        // configuration load and unload. With settings it also reads the switch, the power setting
        // and the limits of the power.
        QFuture<QList<LightSourceState>> readLightSources(const QList<LightSource>& sources, bool settings);
        // Sets a property on the hardware thread. The result is null on success, else the error.
        QFuture<QString> writeProperty(const QString& deviceLabel, const QString& name, const QString& value);
        QStringList availableConfigGroups() const;
        QStringList availableConfigs(const QString& configGroup) const;
        QString currentConfig(const QString& groupName) const;
        QList<ConfigSetting> configPresetSettings(const QString& groupName, const QString& presetName) const;
        bool setConfig(const QString& groupName,
                       const QString& configName,
                       QString* errorMessage = nullptr);
        bool addConfigGroup(const QString& groupName, QString* errorMessage = nullptr);
        bool removeConfigGroup(const QString& groupName, QString* errorMessage = nullptr);
        bool renameConfigGroup(const QString& groupName, const QString& newName, QString* errorMessage = nullptr);
        bool setConfigPreset(const QString& groupName, const QString& presetName,
                             const QList<ConfigSetting>& settings, QString* errorMessage = nullptr);
        bool removeConfigPreset(const QString& groupName, const QString& presetName, QString* errorMessage = nullptr);
        bool renameConfigPreset(const QString& groupName, const QString& presetName,
                                const QString& newName, QString* errorMessage = nullptr);
        bool setConfigGroupProperties(const QString& groupName, const QList<ConfigSetting>& properties,
                                      QString* errorMessage = nullptr);


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
        QString realTimeProcessingSource() const;
        bool setRealTimeProcessingSource(const QString& cameraId);
        // The recipe of the shared pipeline, or of the pipeline that a camera runs
        ProcessingRecipe processingRecipe(const QString& cameraId = QString()) const;
        bool applyProcessingRecipe(const ProcessingRecipe& recipe, QString* errorMessage = nullptr);
        ImageFrame processFrame(const ImageFrame& frame) const;
        ImageFrame processFrameFrom(int startModuleIndex, const ImageFrame& frame) const;
        ImageFrame processFrameThrough(int endModuleIndex, const ImageFrame& frame) const;
        QList<ProcessingModuleDescriptor> availableProcessingModules() const;
        // Python processing modules come from the scripts the user adds. Core reads a script with
        // python -m scopeone.processing describe, lists its modules and remembers the script, so its
        // modules are there again after a restart. Adding a script again reads it again. Returns the
        // module ids of the script.
        QStringList addPythonScript(const QString& path, QString* errorMessage = nullptr);
        // Fails while a pipeline has a step of the script
        bool removePythonScript(const QString& path, QString* errorMessage = nullptr);
        QStringList pythonScripts() const;
        // The script of a Python module, empty for a module of Core
        QString pythonScriptOf(const QString& moduleId) const;
        std::unique_ptr<ProcessingModule> createProcessingModule(const QString& moduleId) const;
        // A pipeline made of copies of the steps that the camera runs, which are those of the shared
        // pipeline when no camera is named or the camera has none of its own
        std::unique_ptr<ProcessingPipeline> createProcessingPipeline(const QString& cameraId = QString()) const;
        // The steps a camera runs: its own, or those of the shared pipeline. An empty camera id names the
        // shared pipeline, which the cameras without a pipeline of their own use. The functions that
        // change steps work on the pipeline of the camera itself, and fail for a camera that has none.
        QList<ProcessingModuleInfo> processingModules(const QString& cameraId = QString()) const;
        bool addProcessingModule(const QString& moduleId, const QString& cameraId = QString());
        bool removeProcessingModule(int index, const QString& cameraId = QString());
        bool moveProcessingModule(int from, int to, const QString& cameraId = QString());
        bool setProcessingModuleEnabled(int index, bool enabled, const QString& cameraId = QString());
        bool setProcessingModuleParameters(int index, const QVariantMap& parameters,
                                           const QString& cameraId = QString());
        bool resetProcessingModuleState(int index, const QString& cameraId = QString());
        // Whether a camera has a pipeline of its own, and giving it one (a copy of the shared pipeline to
        // start from) or taking it away so that the camera uses the shared pipeline again
        bool hasOwnProcessingPipeline(const QString& cameraId) const;
        bool setOwnProcessingPipeline(const QString& cameraId, bool own);
        QStringList camerasWithOwnProcessingPipeline() const;
        quint64 requestImageProcessing(const ImageFrame& frame,
                                       const QString& sourceId = QString());
        quint64 requestRecordingSessionStackProcessing(const QString& sessionId,
                                                       const QString& cameraId);
        quint64 requestLayerStackProcessing(const QString& layerKey);
        bool cancelProcessingRequest(quint64 requestId);
        // Replaces the triggers, or keeps them and fails when one is invalid
        bool setTriggers(const QList<Trigger>& triggers, QString* errorMessage = nullptr);
        QList<Trigger> triggers() const { return m_triggers; }
        // Turns on the hook points of the experiment events; no points turns the hooks off. They
        // apply to experiments that start afterwards. A request without a reply within timeoutMs
        // continues with the event unchanged. Turning them off releases a waiting event.
        void setHooks(const QList<HookPoint>& points, int timeoutMs = 5000);
        QList<HookPoint> hooks() const;
        int hookTimeoutMs() const;
        // Thread safe. Answers request hookId, and is false when no event waits on it.
        bool replyHook(quint64 hookId, const HookReply& reply);
        // Appends to the action log, which keeps the last 10000 entries
        void logAction(const QString& origin,
                       const QString& operation,
                       const QJsonObject& parameters,
                       bool success,
                       const QString& error = QString());
        // The entries at or after since, all of them for an invalid since
        QList<ActionLogEntry> actionLog(const QDateTime& since = QDateTime()) const;


        void setRecordingMaxPendingWriteBytes(qint64 bytes);
        qint64 recordingMaxPendingWriteBytes() const;
        RecordingProgress recordingProgress() const;
        RecordingWriterStatus recordingWriterStatus() const;
        bool startRecording(const ExperimentPlan& plan, const QStringList& activeCameraIds,
                            QString* errorMessage = nullptr);
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
        bool saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session);
        bool saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session,
                                  const RecordingSaveOptions& saveOptions);
        bool saveRecordingSessionCamera(const std::shared_ptr<RecordingSessionData>& session,
                                        const QString& cameraId,
                                        const RecordingSaveOptions& saveOptions,
                                        const ExperimentDocument* presentation = nullptr);
        quint64 requestRecordingSessionFrame(
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& cameraId,
            int index);

    signals:
        void configurationLoadFinished(bool success,
                                       const LoadConfigResult& result,
                                       const QString& errorMessage);
        void configurationUnloadFinished(bool success, const QString& errorMessage);
        void hardwareConfigurationChanged();
        void hardwareDevicesChanged();
        void deviceStateChanged();
        void stagePositionChanged();
        void stageMoveFinished(quint64 commandId,
                               const QString& deviceLabel,
                               bool success,
                               const QString& errorMessage);
        void newRawFrameReady(const ImageFrame& frame);
        void rawFramesAcquired(const QString& cameraId, quint64 frameCount);
        void previewRawFrameReady(const ImageFrame& frame);
        void previewStateChanged(bool running);
        // The signal sources or DAQ devices changed as a driver was registered or removed
        void signalSourcesChanged();
        void daqDevicesChanged();
        void signalTimeSeriesReady(const TimeSeriesChunk& chunk);
        void signalSourceStateChanged(const QString& sourceId,
                                      SignalSourceState state,
                                      const QString& message);
        void signalSourceError(const QString& sourceId,
                               const QString& errorMessage);
        void scanImageSessionReady(
            const std::shared_ptr<RecordingSessionData>& session);
        void daqStateChanged(const QString& deviceId,
                             DaqState state,
                             const QString& message);
        void daqError(const QString& deviceId,
                      const QString& errorMessage);
        void daqInputDataReady(const DaqInputChunk& chunk);
        void previewProcessedFrameReady(const ImageFrame& frame);
        void staticFramePublished(const QString& sourceId,
                                  const QString& displayName,
                                  const ImageFrame& frame);
        void previewToolFrameReady(const ImageFrame& frame);
        void staticFrameRemoved(const QString& sourceId);
        void staticFramesCleared();
        void liveFramesCleared(const QString& cameraId);
        void processedFramesCleared();
        void layerHistogramReady(const QString& layerKey, const HistogramStats& stats);
        void layerAnalysisCleared(const QString& layerKey);
        void lineProfileUpdated(const QString& cameraId, bool processed, const QVector<int>& values);
        void layerLineProfileUpdated(const QString& layerKey, const QVector<int>& values);
        void lineProfileCleared();
        void particleDetectionFinished(quint64 requestId,
                                       const QString& layerKey,
                                       const ParticleDetectionResult& result,
                                       const QString& errorMessage);
        void stageMosaicProgress(int completedTiles, int totalTiles, const QString& message);
        void stageMosaicFrameUpdated(const ImageFrame& frame);
        void stageMosaicFinished(
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& message,
            bool canceled);
        void processingError(const QString& errorMessage);
        // The measurements a processing module made on one frame of a camera
        void measurementsReady(const QString& cameraId,
                               const QString& moduleId,
                               qint64 frameIndex,
                               const QVariantList& measurements);
        // A trigger fired on a measurement and performed its action
        void triggerFired(int index,
                          const QVariantMap& measurement,
                          const QString& action,
                          bool success,
                          const QString& error);
        // An experiment event waits at a hook point for replyHook. Emitted on the acquisition
        // thread. data holds the event, and after the event also its frames.
        void hookRequested(quint64 hookId, const QString& point, const QVariantMap& data);
        void processingModulesChanged();
        // A Python script was added or removed, which changed availableProcessingModules
        void availableProcessingModulesChanged();
        void processingModuleParametersChanged(int index);
        void processingSettingsChanged();
        void imageProcessingFinished(quint64 requestId,
                                     const QString& sourceId,
                                     const ImageFrame& frame,
                                     const QString& errorMessage);
        void stackProcessingProgress(quint64 requestId, qint64 completed, qint64 total);
        void stackProcessingFinished(
            quint64 requestId,
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& errorMessage);
        void layerStackProcessingFinished(
            quint64 requestId,
            const QString& outputLayerKey,
            const QString& errorMessage);

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
        void recordingSessionCameraSaveFinished(
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& cameraId,
            bool success,
            const QString& message);
        void recordingSessionClosed(const QString& sessionId);
        void recordingSessionsChanged();
        void recordingSessionFrameReady(
            quint64 requestId,
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& cameraId,
            int index,
            const ImageFrame& frame);
        void imageOpenProgress(const QString& filePath, int percent, const QString& statusText);
        void imageOpenFinished(
            const QString& filePath,
            const std::shared_ptr<RecordingSessionData>& session,
            const QString& errorMessage);
        void layerFrameRatesUpdated(const QMap<QString, double>& frameRates);

     private:
        bool checkPlanHardware(const ExperimentPlan& plan, QString* errorMessage) const;
        void handleMeasurements(const QString& cameraId,
                                const QString& moduleId,
                                qint64 frameIndex,
                                const QVariantList& measurements);
        bool runTrigger(const Trigger& trigger, const QString& cameraId, QString* errorMessage);
        struct Managers;

        enum class FrameGraphStream
        {
            Raw,
            Processed,
            Tool,
            Static,
            External
        };

        class FrameGraph
        {
        public:
            void clear();
            ImageFrame publishLatest(FrameGraphStream stream, const ImageFrame& frame);
            ImageFrame publishLatest(FrameGraphStream stream, const QString& sourceId, const ImageFrame& frame);
            ImageFrame latest(FrameGraphStream stream, const QString& sourceId) const;
            void remove(FrameGraphStream stream, const QString& sourceId);
            void clear(FrameGraphStream stream);

        private:
            QHash<QString, ImageFrame>& latestMap(FrameGraphStream stream);
            const QHash<QString, ImageFrame>& latestMap(FrameGraphStream stream) const;

            QHash<QString, ImageFrame> m_rawFrames;
            QHash<QString, ImageFrame> m_processedFrames;
            QHash<QString, ImageFrame> m_toolFrames;
            QHash<QString, ImageFrame> m_staticFrames;
            QHash<QString, ImageFrame> m_externalFrames;
        };

        struct HistogramJobState
        {
            bool inFlight{false};
            bool retryScheduled{false};
            QElapsedTimer lastScheduledTimer;
            quint64 activeSequence{0};
            ImageFrame queuedFrame;
        };

        void unloadConfigurationForShutdown();
        void applyLoadedConfiguration(const QString& configPath,
                                      const LoadConfigResult& result);
        void synchronizeCameraIdsFromRegistry();
        void logConfigurationLoaded(const LoadConfigResult& result) const;
        void finishConfigurationLoadFailure(const LoadConfigResult& result,
                                            const QString& errorMessage);
        void clearConfigurationRuntime(bool notify, bool stopCameras);
        // Stops the DriverHosts of the cameras and devices on the hardware thread, as they can take
        // a second to exit, and then calls next
        void stopDriverHosts(std::function<void()> next, bool applyShutdown = false);
        void startConfigurationLoadTask(const QString& configPath);
        void startConfigurationLoadCleanupTask(const LoadConfigResult& result,
                                               const QString& errorMessage);
        void startDeviceUnloadAfterFailedLoad(const LoadConfigResult& result,
                                              const QString& errorMessage);
        bool canStartConfigurationOperation();
        bool hardwareBusy(QString* errorMessage) const;
        bool canEditConfiguration(QString* errorMessage) const;
        bool finishConfigurationEdit(bool success);
        internal::ProcessingPipelineDefinition* editableDefinition(const QString& cameraId) const;
        QHash<QString, double> cameraPixelSizesUm(const QStringList& cameraIds) const;
        QHash<QString, CameraSettings> cameraSettings(const QStringList& cameraIds,
                                                      const QJsonObject& deviceProperties) const;
        void startConfigurationUnloadTask();
        quint64 queueStageMove(
            const QString& deviceLabel,
            std::function<bool(QString*)> command);
        bool isPropertyPreInit(const QString& deviceLabel, const QString& name) const;
        void ensureSceneLayer(const QString& layerKey,
                              const QString& sourceId,
                              const QString& name,
                              DocumentLayerKind kind);
        void handleIncomingRawFrame(const ImageFrame& frame);
        void handleSignalTimeSeries(const TimeSeriesChunk& chunk);
        void handleTimestampedSignalEvents(const TimestampedEventChunk& chunk);
        void publishScanFrames(const QString& sourceId,
                               const QList<ImageFrame>& frames);
        void finishScanImageSession(const QString& sourceId,
                                    ExperimentRunState finalState,
                                    const QString& message);
        void submitProcessingFrame(const ImageFrame& frame, quint64 processingToken = 0);
        void handleProcessedFrame(const ImageFrame& frame);
        void recordLayerFrame(const QString& layerKey, quint64 count = 1);
        void updateLayerFrameRates();
        void flushProcessedFrames();
        void queuePreviewRawFrame(const ImageFrame& frame);
        void schedulePreviewFlush();
        void flushPreviewFrames();
        bool histogramUpdatesEnabled(const QString& layerKey) const;
        void scheduleHistogramStats(const QString& layerKey, const ImageFrame& frame);
        void clearLayerAnalysis(const QString& layerKey);
        void clearLayerAnalysisByPrefix(const QString& prefix);
        void updateLineProfile(const QString& cameraId,
                               bool processed,
                               bool toolSource,
                               const ImageFrame& frame);
        bool updateStaticLineProfile(const QString& sourceId, const ImageFrame& frame);
        void setLineProfile(const QString& cameraId,
                            const QPoint& start,
                            const QPoint& end,
                            bool processed,
                            bool toolSource = false);
        void setStaticLineProfile(const QString& sourceId,
                                  const QPoint& start,
                                  const QPoint& end);
        void clearLineProfile();
        void syncLineProfileFromScene();
        void registerRecordingSession(const std::shared_ptr<RecordingSessionData>& session);
        void finalizeActiveExperiment(const std::shared_ptr<RecordingSessionData>& session);
        bool queueRecordingSessionSave(
            const std::shared_ptr<RecordingSessionData>& sourceSession,
            const std::shared_ptr<RecordingSessionData>& saveSession,
            const QString& cameraId = QString());

        struct ActiveLineProfile
        {
            QString sourceId;
            QPoint start;
            QPoint end;
            bool processed{false};
            bool toolSource{false};
            bool staticSource{false};
            bool active{false};
        };

        std::unique_ptr<Managers> m_managers;
        ImageSceneModel* m_imageSceneModel{nullptr};
        QStringList m_cameraIds;
        QString m_loadedConfigPath;
        QString m_loadedConfigSha256;
        QString m_configurationError;
        QStringList m_configurationFailedDevices;
        ActiveLineProfile m_activeLineProfile;
        FrameGraph m_frameGraph;
        QHash<QString, ImageFrame> m_pendingPreviewRawFrames;
        QHash<QString, ImageFrame> m_pendingPreviewProcessedFrames;
        QHash<QString, ImageFrame> m_pendingPreviewToolFrames;
        QHash<QString, HistogramJobState> m_histogramJobStates;
        QHash<QString, HistogramStats> m_latestHistogramStats;
        std::unique_ptr<QThreadPool> m_histogramThreadPool;
        std::unique_ptr<QThreadPool> m_hardwareThreadPool;
        std::unique_ptr<QThreadPool> m_analysisThreadPool;
        std::unique_ptr<QThreadPool> m_sessionFrameThreadPool;
        std::unique_ptr<QThreadPool> m_offlineProcessingThreadPool;
        QString m_activeHistogramLayerKey;
        quint64 m_nextHistogramSequence{0};
        QElapsedTimer m_lineProfileUpdateTimer;
        bool m_previewFlushQueued{false};
        QTimer* m_layerFrameRateTimer{nullptr};
        mutable QMutex m_layerFrameRateMutex;
        QElapsedTimer m_layerFrameRateElapsed;
        QHash<QString, quint64> m_layerFrameCounts;
        QMap<QString, double> m_layerFrameRates;
        QSet<const RecordingSessionData*> m_sessionsSaving;
        enum class ConfigurationState
        {
            Unloaded,
            Loading,
            Loaded,
            PartiallyLoaded,
            Unloading,
            Failed
        };
        ConfigurationState m_configurationState{ConfigurationState::Unloaded};
        bool m_configurationOperationRunning{false};
        int m_pendingStageCommands{0};
        quint64 m_nextStageCommandId{0};
        quint64 m_nextAnalysisRequestId{0};
        quint64 m_analysisGeneration{0};
        quint64 m_nextSessionFrameRequestId{0};
        QString m_realTimeProcessingSource;
        QHash<quint64, std::shared_ptr<std::atomic_bool>> m_processingRequestCancelTokens;
        quint64 m_nextProcessingRequestId{0};
        QHash<QString, std::vector<ImageFrame>> m_layerStacks;
        QList<Trigger> m_triggers;
        // The pre-trigger buffers of the triggers, kept to restore them when new ones fail
        QHash<QString, int> m_preTriggerFrameCounts;
        QHash<QString, qint64> m_preTriggerByteLimits;
        // When each trigger last fired on m_triggerClock, or -1
        QList<qint64> m_triggerFiredMs;
        QElapsedTimer m_triggerClock;
        // Counts setTriggers calls, so a trigger loop sees when an action replaced the triggers
        quint64 m_triggerGeneration{0};
        // Set while a trigger action runs
        bool m_runningTrigger{false};
        // The processing error logged last, so a module that fails on every frame logs it once
        QString m_lastProcessingError;
        static constexpr qsizetype kActionLogSize = 10000;
        QList<ActionLogEntry> m_actionLog;
        // Entries ever appended, and their count when the last configuration finished loading,
        // where the log of a recording starts
        quint64 m_actionLogCount{0};
        quint64 m_configLoadedLogCount{0};
    };
}

Q_DECLARE_METATYPE(std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::RecordingWriterStatus)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::HistogramStats)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::LoadConfigResult)

Q_DECLARE_METATYPE(scopeone::core::ScopeOneCore::ParticleDetectionResult)
