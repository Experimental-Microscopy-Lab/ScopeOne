#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
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
        XY = 2
    };

    enum class ProcessingModuleKind
    {
        FFT = 0,
        BackgroundCalibration = 2,
        SpatiotemporalBinning = 3,
        GaussianBlur = 4,
        DifferentialRolling = 5,
        Unknown = 255
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
        ProcessingModuleKind kind{ProcessingModuleKind::Unknown};
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
        int framesPerBurst{1};
        bool burstMode{false};
        int targetBursts{1};
        double burstIntervalMs{0.0};
        double mdaIntervalMs{0.0};
        double exposureMs{0.0};
        double pixelSizeUm{0.0};
        std::vector<RecordingAxis> order{RecordingAxis::Time, RecordingAxis::Z, RecordingAxis::XY};
        std::vector<QPointF> positions;
        std::vector<double> zPositions;
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
    SCOPEONE_CORE_EXPORT QString processingModuleKindName(ProcessingModuleKind kind);
    SCOPEONE_CORE_EXPORT QString experimentRunStateName(ExperimentRunState state);
    SCOPEONE_CORE_EXPORT QString documentLayerKindName(DocumentLayerKind kind);
    SCOPEONE_CORE_EXPORT QString documentMarkupTypeName(DocumentMarkupType type);
    SCOPEONE_CORE_EXPORT QString documentMarkupRoleName(DocumentMarkupRole role);

    SCOPEONE_CORE_EXPORT bool validateExperimentPlan(const ExperimentPlan& plan, QString* errorMessage = nullptr);
    SCOPEONE_CORE_EXPORT bool validateExperimentDocument(const ExperimentDocument& document,
                                                         QString* errorMessage = nullptr);
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
