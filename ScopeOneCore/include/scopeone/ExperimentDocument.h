#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include "scopeone/HardwareCapabilities.h"
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
#include <optional>
#include <vector>

#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

namespace scopeone::core
{
    inline constexpr int kExperimentDocumentSchemaVersion = 2;
    inline constexpr int kProcessingModuleSchemaVersion = 1;

    enum class RecordingFormat
    {
        Tiff = 0,
        Binary = 1,
        OmeTiff = 2,
        OmeZarr = 3
    };

    enum class RecordingAxis
    {
        Time = 0,
        Z = 1,
        XY = 2,
        Channel = 3
    };

    // One channel of a multi-channel acquisition. Like a Micro-Manager channel it is a preset
    // of a configuration group that is applied before the image is taken.
    struct ChannelSetting
    {
        QString preset;
        // Zero keeps the exposure of the plan
        double exposureMs{0.0};
        // Focus offset of this channel in micrometers
        double zOffsetUm{0.0};
        // False images this channel only at the middle Z position
        bool zStack{true};
        // Images this channel at every (skipFrames + 1)th time point
        int skipFrames{0};
        // Display colour of the channel as #RRGGBB. Empty leaves the choice to the viewer.
        QString color;
        // The device properties that the preset sets, read from the hardware when the
        // acquisition starts and stored with the images. Not part of the experiment document.
        std::vector<ConfigSetting> presetSettings;
        // Camera that takes this channel. Empty uses every camera of the plan. The other
        // cameras leave the planes of the channel empty.
        QString cameraId;
    };

    enum class ProcessingBitDepth
    {
        Bit8 = 8,
        Bit16 = 16
    };

    enum class ExperimentRunState
    {
        Draft,
        Running,
        Completed,
        Canceled,
        Failed
    };

    enum class DocumentLayerKind
    {
        Raw,
        Processed,
        Tool,
        Static,
        Gallery
    };

    enum class DocumentMarkupType
    {
        Line,
        Rect
    };

    enum class DocumentMarkupRole
    {
        Generic,
        CrossSection,
        Roi,
        Measurement
    };

    struct ProcessingModuleRecipe
    {
        QString moduleId;
        int schemaVersion{kProcessingModuleSchemaVersion};
        QVariantMap parameters;
    };

    struct ProcessingRecipe
    {
        ProcessingBitDepth bitDepth{ProcessingBitDepth::Bit16};
        QList<ProcessingModuleRecipe> modules;
    };

    struct ExperimentPlan
    {
        int schemaVersion{kExperimentDocumentSchemaVersion};
        QString experimentId;
        QStringList cameraIds;
        RecordingFormat format{RecordingFormat::OmeTiff};
        bool streamToDisk{true};
        bool enableCompression{false};
        int compressionLevel{6};
        // Write OME-Zarr resolution levels so large images open quickly in viewers
        bool enablePyramid{false};
        int framesPerBurst{1};
        bool burstMode{false};
        int targetBursts{1};
        double burstIntervalMs{0.0};
        double mdaIntervalMs{0.0};
        // Interval after each time point except the last one, for acquisitions whose time
        // points are not evenly spaced. Either empty, or framesPerBurst - 1 long and then
        // used instead of mdaIntervalMs.
        std::vector<double> timeIntervalsMs;
        double exposureMs{0.0};
        double pixelSizeUm{0.0};
        std::vector<RecordingAxis> order{RecordingAxis::Time, RecordingAxis::Z, RecordingAxis::XY};
        std::vector<QPointF> positions;
        // Names of the positions and the focus position of each one. Either list is empty or
        // as long as positions; a position without a name is called Position N and one without
        // a focus position keeps the focus of the Z positions. With a focus position, the Z
        // positions are offsets from it.
        std::vector<QString> positionLabels;
        std::vector<std::optional<double>> positionZ;
        std::vector<double> zPositions;
        // Configuration group whose presets are the channels
        QString channelGroup;
        std::vector<ChannelSetting> channels;
        // Keep the shutter open between the Z positions or between the channels, instead of
        // opening it for every image
        bool keepShutterOpenSlices{false};
        bool keepShutterOpenChannels{false};
        // Run the autofocus device before the first image of each position at a time point.
        // With autofocusEveryFrames above 1 it runs at every Nth time point only. The Z
        // positions that follow are shifted by the distance the autofocus moved the focus.
        bool autofocus{false};
        int autofocusEveryFrames{0};
        // Let the hardware time the Z slices: the focus device steps through its positions on
        // the triggers of the camera, which takes the slices in one sequence acquisition. It is
        // used for the slices of a block where the focus device and the camera both support
        // sequences, and the slices are taken one by one otherwise.
        bool hardwareSequencing{false};
        // Free text that is stored with the acquisition
        QString comment;
        QString saveDir;
        QString baseName;
        QString metadataFileName;
        QString configPath;
        QString configSha256;
        ProcessingRecipe processing;
        QJsonObject userMetadata;
    };

    struct AcquisitionEvent
    {
        quint64 sequenceIndex{0};
        int burstIndex{0};
        int timeIndex{0};
        int zIndex{0};
        int positionIndex{0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        bool hasXY{false};
        bool hasZ{false};
        double exposureMs{0.0};
        qint64 minimumStartTimeMs{0};
        QStringList cameraIds;
        // Channel of a multi-channel acquisition; the preset is empty without channels
        int channelIndex{0};
        QString channelGroup;
        QString channelPreset;
        // Focus offset of the channel from the focus position at the start of the
        // acquisition, used when the plan has no Z positions of its own
        double zOffsetUm{0.0};
        // The shutter is opened before this event and closed after it. Derived from the plan
        // when the events are built, so it is not part of the experiment document.
        bool openShutterBefore{false};
        bool closeShutterAfter{false};
        // The autofocus runs before this event
        bool autofocusBefore{false};
        // The Z slices of the plan may be taken in one hardware sequence. Derived from the plan.
        bool hardwareSequencing{false};
    };

    struct FrameRecord
    {
        QString cameraId;
        int width{0};
        int height{0};
        int bitsPerSample{0};
        ImagePixelFormat pixelFormat{ImagePixelFormat::Invalid};
        quint64 frameIndex{0};
        quint64 timestampNs{0};
        int sourceRoiX{0};
        int sourceRoiY{0};
        int sourceRoiWidth{0};
        int sourceRoiHeight{0};
    };

    struct AcquisitionEventRecord
    {
        AcquisitionEvent event;
        quint64 startedTimestampNs{0};
        quint64 completedTimestampNs{0};
        bool succeeded{false};
        QString errorMessage;
        QHash<QString, FrameRecord> frames;
    };

    struct SoftwareSnapshot
    {
        QString applicationVersion;
        QString coreVersion;
        QString mmCoreVersion;
        QString libTiffVersion;
        QString zlibVersion;
        QString operatingSystem;
    };

    struct RecordingFileManifest
    {
        QString rawPath;
        QString frameInfoPath;
        qint64 framesWritten{0};
    };

    struct RecordingOutputManifest
    {
        bool streamedToDisk{false};
        QHash<QString, RecordingFileManifest> files;

        void clearFiles() { files.clear(); }
    };

    struct SCOPEONE_CORE_EXPORT ImageTransform2D
    {
        double m11{1.0};
        double m12{0.0};
        double m21{0.0};
        double m22{1.0};
        double dx{0.0};
        double dy{0.0};

        QPointF map(const QPointF& point) const;
        bool inverted(ImageTransform2D& inverse) const;
    };

    struct LayerDisplayState
    {
        bool visible{false};
        int opacityPercent{100};
        double gamma{1.0};
        QString colormap{QStringLiteral("Gray")};
        QString blending{QStringLiteral("Translucent")};
        int levelMin{0};
        int levelMax{255};
        int levelDomainMax{255};
    };

    struct DocumentLayer
    {
        QString id;
        QString sourceId;
        QString name;
        DocumentLayerKind kind{DocumentLayerKind::Raw};
        int width{0};
        int height{0};
        ImageTransform2D pixelToSensor;
        LayerDisplayState display;
    };

    struct DocumentMarkup
    {
        QString id;
        DocumentMarkupType type{DocumentMarkupType::Line};
        DocumentMarkupRole role{DocumentMarkupRole::Generic};
        QString layerId;
        QString label;
        QPointF start;
        QPointF end;
        QRectF rect;
        bool visible{true};
        bool selected{false};
    };

    struct ExperimentDocument
    {
        int schemaVersion{kExperimentDocumentSchemaVersion};
        ExperimentPlan plan;
        ExperimentRunState runState{ExperimentRunState::Draft};
        quint64 startedTimestampNs{0};
        quint64 completedTimestampNs{0};
        QString errorMessage;
        SoftwareSnapshot software;
        QJsonObject deviceProperties;
        QList<AcquisitionEventRecord> events;
        RecordingOutputManifest output;
        QList<DocumentLayer> layers;
        QList<DocumentMarkup> markups;

        RecordingFileManifest& ensureFile(const QString& cameraId)
        {
            return output.files[cameraId.trimmed()];
        }

        void clearOutput() { output.clearFiles(); }
    };

    SCOPEONE_CORE_EXPORT QString recordingAxisName(RecordingAxis axis);
    SCOPEONE_CORE_EXPORT QString experimentRunStateName(ExperimentRunState state);
    SCOPEONE_CORE_EXPORT QString documentLayerKindName(DocumentLayerKind kind);
    SCOPEONE_CORE_EXPORT QString documentMarkupTypeName(DocumentMarkupType type);
    SCOPEONE_CORE_EXPORT QString documentMarkupRoleName(DocumentMarkupRole role);

    SCOPEONE_CORE_EXPORT bool validateExperimentPlan(const ExperimentPlan& plan, QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT bool validateExperimentDocument(const ExperimentDocument& document,
                                                         QString* errorMessage = nullptr);
    // Name of a position as it appears in the output and the OME metadata
    SCOPEONE_CORE_EXPORT QString positionName(const ExperimentPlan& plan, int positionIndex);

    // Order of the time, Z and channel axes of the plan from the slowest to the fastest
    // changing one, as the letters T, Z and C. Every letter appears exactly once.
    SCOPEONE_CORE_EXPORT QString omeAcquisitionOrder(const ExperimentPlan& plan);

    // Position of an event among the planes of one OME image, counting from zero
    SCOPEONE_CORE_EXPORT quint64 omePlaneIndex(const ExperimentPlan& plan, const AcquisitionEvent& event);

    SCOPEONE_CORE_EXPORT QList<AcquisitionEvent> buildAcquisitionEvents(const ExperimentPlan& plan,
                                                                       int burstIndex = 0,
                                                                       QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT FrameRecord frameRecordFromImageFrame(const ImageFrame& frame);
    SCOPEONE_CORE_EXPORT ImageTransform2D imagePixelToSensorTransform(const ImageFrame& frame);

    SCOPEONE_CORE_EXPORT QJsonObject experimentPlanToJson(const ExperimentPlan& plan);
    SCOPEONE_CORE_EXPORT bool experimentPlanFromJson(const QJsonObject& object,
                                                     ExperimentPlan& plan,
                                                     QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT QJsonObject experimentDocumentToJson(const ExperimentDocument& document);
    SCOPEONE_CORE_EXPORT bool experimentDocumentFromJson(const QJsonObject& object,
                                                         ExperimentDocument& document,
                                                         QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT QByteArray serializeExperimentDocument(const ExperimentDocument& document);
    SCOPEONE_CORE_EXPORT bool deserializeExperimentDocument(const QByteArray& json,
                                                            ExperimentDocument& document,
                                                            QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT bool saveExperimentDocument(const QString& filePath,
                                                     const ExperimentDocument& document,
                                                     QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT bool loadExperimentDocument(const QString& filePath,
                                                     ExperimentDocument& document,
                                                     QString* errorMessage = nullptr);
}
