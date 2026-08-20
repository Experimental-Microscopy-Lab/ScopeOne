#include "ScopeOneLocalApiServer.h"

#include "scopeone/ImageSceneModel.h"
#include "PreviewWidget.h"
#include "scopeone/ScopeOneCore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <QtGlobal>
#include <QtConcurrent>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace scopeone::ui
{
    using ImageSceneModel = scopeone::core::ImageSceneModel;

    namespace
    {
        constexpr quint32 kMaxMessageBytes = 64 * 1024 * 1024;
#if defined(_WIN32)
        const QString kServerName = QStringLiteral(R"(\\.\pipe\ScopeOne.Api.local)");
#else
        // QLocalServer turns this into a unix socket at <tempdir>/ScopeOne.Api.local
        const QString kServerName = QStringLiteral("ScopeOne.Api.local");
        // POSIX shared memory object, visible to external clients at /dev/shm/ScopeOne.Api.frame
        const char* const kPosixFrameShmName = "/ScopeOne.Api.frame";
#endif
        const QString kFrameMappingName = QStringLiteral("ScopeOne.Api.frame");

        // Encodes one JSON object with a little endian size prefix
        QByteArray encodeMessage(const QJsonObject& message)
        {
            const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
            if (payload.isEmpty() || payload.size() > kMaxMessageBytes)
            {
                return {};
            }
            QByteArray framed;
            framed.resize(static_cast<int>(sizeof(quint32)));
            qToLittleEndian<quint32>(static_cast<quint32>(payload.size()),
                                     reinterpret_cast<uchar*>(framed.data()));
            framed += payload;
            return framed;
        }

        enum class DecodeResult
        {
            Incomplete,
            Complete,
            Error
        };

        // Decodes one framed JSON message from a socket buffer
        DecodeResult tryDecodeMessage(QByteArray& buffer, QJsonObject& message)
        {
            if (buffer.size() < static_cast<int>(sizeof(quint32)))
            {
                return DecodeResult::Incomplete;
            }

            const quint32 payloadSize =
                qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData()));
            if (payloadSize == 0 || payloadSize > kMaxMessageBytes)
            {
                buffer.clear();
                return DecodeResult::Error;
            }

            const int frameSize = static_cast<int>(sizeof(quint32) + payloadSize);
            if (buffer.size() < frameSize)
            {
                return DecodeResult::Incomplete;
            }

            const QByteArray payload = buffer.mid(static_cast<int>(sizeof(quint32)), static_cast<int>(payloadSize));
            buffer.remove(0, frameSize);

            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                return DecodeResult::Error;
            }

            message = document.object();
            return DecodeResult::Complete;
        }

        // Creates a standard local API response object
        QJsonObject makeResponse(const QString& type, bool ok)
        {
            QJsonObject response;
            response.insert(QStringLiteral("type"), type);
            response.insert(QStringLiteral("ok"), ok);
            return response;
        }

        // Converts one markup to local API JSON
        QJsonObject markupToJson(const ImageSceneModel::Markup& markup)
        {
            QJsonObject object;
            object.insert(QStringLiteral("id"), markup.id);
            object.insert(QStringLiteral("type"), ImageSceneModel::typeName(markup.type));
            object.insert(QStringLiteral("role"), ImageSceneModel::roleName(markup.role));
            object.insert(QStringLiteral("layerKey"), markup.layerKey);
            object.insert(QStringLiteral("layerKind"), ImageSceneModel::layerKindName(markup.layerKind));
            object.insert(QStringLiteral("sourceId"), markup.sourceId);
            object.insert(QStringLiteral("coordinateSpace"), QStringLiteral("image"));
            object.insert(QStringLiteral("label"), markup.label);
            object.insert(QStringLiteral("visible"), markup.visible);
            object.insert(QStringLiteral("selected"), markup.selected);
            if (markup.type == ImageSceneModel::MarkupType::Line)
            {
                object.insert(QStringLiteral("x1"), markup.start.x());
                object.insert(QStringLiteral("y1"), markup.start.y());
                object.insert(QStringLiteral("x2"), markup.end.x());
                object.insert(QStringLiteral("y2"), markup.end.y());
            }
            else if (markup.type == ImageSceneModel::MarkupType::Rect)
            {
                object.insert(QStringLiteral("x"), markup.rect.x());
                object.insert(QStringLiteral("y"), markup.rect.y());
                object.insert(QStringLiteral("width"), markup.rect.width());
                object.insert(QStringLiteral("height"), markup.rect.height());
            }
            return object;
        }

        ImageSceneModel::MarkupRole markupRoleFromJson(const QJsonValue& value)
        {
            QString roleName = value.toString().trimmed().toLower();
            roleName.remove(QLatin1Char('_'));
            roleName.remove(QLatin1Char('-'));
            roleName.remove(QLatin1Char(' '));
            if (roleName == QStringLiteral("crosssection") || roleName == QStringLiteral("lineprofile"))
            {
                return ImageSceneModel::MarkupRole::CrossSection;
            }
            if (roleName == QStringLiteral("roi"))
            {
                return ImageSceneModel::MarkupRole::Roi;
            }
            if (roleName == QStringLiteral("measurement") || roleName == QStringLiteral("measure"))
            {
                return ImageSceneModel::MarkupRole::Measurement;
            }
            return ImageSceneModel::MarkupRole::Generic;
        }

        // Converts device property metadata to JSON
        QJsonObject propertyInfoToJson(const scopeone::core::ScopeOneCore::DevicePropertyInfo& info)
        {
            QJsonObject object;
            object.insert(QStringLiteral("name"), info.name());
            object.insert(QStringLiteral("value"), info.value());
            object.insert(QStringLiteral("type"), info.type());
            object.insert(QStringLiteral("readOnly"), info.isReadOnly());
            object.insert(QStringLiteral("preInit"), info.isPreInit());
            object.insert(QStringLiteral("allowedValues"), QJsonArray::fromStringList(info.allowedValues()));
            object.insert(QStringLiteral("hasLimits"), info.hasLimits());
            if (info.hasLimits())
            {
                object.insert(QStringLiteral("lowerLimit"), info.lowerLimit());
                object.insert(QStringLiteral("upperLimit"), info.upperLimit());
            }
            return object;
        }

        QString processingModuleIdFromJson(const QJsonValue& value)
        {
            return value.isString() ? value.toString().trimmed() : QString{};
        }

        QJsonObject processingModuleToJson(
            int index,
            const scopeone::core::ScopeOneCore::ProcessingModuleInfo& info)
        {
            QJsonObject object;
            object.insert(QStringLiteral("index"), index);
            object.insert(QStringLiteral("kind"), info.id());
            object.insert(QStringLiteral("name"), info.name());
            object.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(info.parameters()));
            return object;
        }

        QJsonObject processingModuleDescriptorToJson(
            const scopeone::core::ProcessingModuleDescriptor& descriptor)
        {
            QJsonObject object;
            object.insert(QStringLiteral("id"), descriptor.id);
            object.insert(QStringLiteral("name"), descriptor.name);
            object.insert(QStringLiteral("schemaVersion"), descriptor.schemaVersion);
            QJsonArray parameters;
            for (const auto& parameter : descriptor.parameters)
            {
                QJsonObject item;
                item.insert(QStringLiteral("key"), parameter.key);
                item.insert(QStringLiteral("name"), parameter.name);
                item.insert(QStringLiteral("type"), static_cast<int>(parameter.type));
                item.insert(QStringLiteral("default"), QJsonValue::fromVariant(parameter.defaultValue));
                item.insert(QStringLiteral("minimum"), QJsonValue::fromVariant(parameter.minimum));
                item.insert(QStringLiteral("maximum"), QJsonValue::fromVariant(parameter.maximum));
                parameters.append(item);
            }
            object.insert(QStringLiteral("parameters"), parameters);
            return object;
        }

        QJsonArray processingModulesToJson(
            const QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo>& modules)
        {
            QJsonArray array;
            for (int i = 0; i < modules.size(); ++i)
            {
                array.append(processingModuleToJson(i, modules.at(i)));
            }
            return array;
        }

        QJsonObject processingStateToJson(const scopeone::core::ScopeOneCore* core)
        {
            QJsonObject object;
            object.insert(QStringLiteral("bitDepth"), static_cast<int>(core->processingBitDepth()));
            object.insert(QStringLiteral("realTime"), core->isRealTimeProcessingEnabled());
            object.insert(QStringLiteral("modules"), processingModulesToJson(core->processingModules()));
            return object;
        }

        QString stageMosaicStateName(scopeone::core::ScopeOneCore::StageMosaicState state)
        {
            using State = scopeone::core::ScopeOneCore::StageMosaicState;
            switch (state)
            {
            case State::Idle:
                return QStringLiteral("idle");
            case State::Running:
                return QStringLiteral("running");
            case State::Completed:
                return QStringLiteral("completed");
            case State::Canceled:
                return QStringLiteral("canceled");
            case State::Failed:
                return QStringLiteral("failed");
            }
            return QStringLiteral("failed");
        }

        QJsonObject stageMosaicStatusToJson(
            const scopeone::core::ScopeOneCore::StageMosaicStatus& status)
        {
            QJsonObject object;
            object.insert(QStringLiteral("state"), stageMosaicStateName(status.state));
            object.insert(QStringLiteral("completedTiles"), status.completedTiles);
            object.insert(QStringLiteral("totalTiles"), status.totalTiles);
            object.insert(QStringLiteral("message"), status.message);
            object.insert(QStringLiteral("sessionId"), status.sessionId);
            return object;
        }

        QString recordingPhaseName(int phase)
        {
            switch (phase)
            {
            case scopeone::core::kRecordingPhaseIdle:
                return QStringLiteral("idle");
            case scopeone::core::kRecordingPhaseRecording:
                return QStringLiteral("recording");
            case scopeone::core::kRecordingPhaseRecordingBurst:
                return QStringLiteral("recording_burst");
            case scopeone::core::kRecordingPhaseRecordingMda:
                return QStringLiteral("recording_mda");
            case scopeone::core::kRecordingPhaseWaitingNextBurst:
                return QStringLiteral("waiting_next_burst");
            case scopeone::core::kRecordingPhaseStopped:
                return QStringLiteral("stopped");
            }
            return QStringLiteral("idle");
        }

        QJsonObject recordingProgressToJson(
            const scopeone::core::ScopeOneCore::RecordingProgress& progress)
        {
            QJsonObject object;
            object.insert(QStringLiteral("phase"), recordingPhaseName(progress.phase));
            object.insert(QStringLiteral("frameCurrent"), progress.frameCurrent);
            object.insert(QStringLiteral("frameTarget"), progress.frameTarget);
            object.insert(QStringLiteral("burstCurrent"), progress.burstCurrent);
            object.insert(QStringLiteral("burstTarget"), progress.burstTarget);
            object.insert(QStringLiteral("waitRemainingMs"), progress.waitRemainingMs);
            object.insert(QStringLiteral("timeIndex"), progress.timeIndex);
            object.insert(QStringLiteral("timeCount"), progress.timeCount);
            object.insert(QStringLiteral("zIndex"), progress.zIndex);
            object.insert(QStringLiteral("zCount"), progress.zCount);
            object.insert(QStringLiteral("positionIndex"), progress.positionIndex);
            object.insert(QStringLiteral("positionCount"), progress.positionCount);
            object.insert(QStringLiteral("hasXY"), progress.hasXY);
            object.insert(QStringLiteral("x"), progress.x);
            object.insert(QStringLiteral("y"), progress.y);
            object.insert(QStringLiteral("hasZ"), progress.hasZ);
            object.insert(QStringLiteral("z"), progress.z);
            return object;
        }

        QString recordingWriterPhaseName(
            scopeone::core::ScopeOneCore::RecordingWriterPhase phase)
        {
            using Phase = scopeone::core::ScopeOneCore::RecordingWriterPhase;
            switch (phase)
            {
            case Phase::Idle:
                return QStringLiteral("idle");
            case Phase::Starting:
                return QStringLiteral("starting");
            case Phase::Writing:
                return QStringLiteral("writing");
            case Phase::Stopping:
                return QStringLiteral("stopping");
            case Phase::Completed:
                return QStringLiteral("completed");
            case Phase::Failed:
                return QStringLiteral("failed");
            }
            return QStringLiteral("idle");
        }

        QJsonObject recordingWriterStatusToJson(
            const scopeone::core::ScopeOneCore::RecordingWriterStatus& status)
        {
            QJsonObject object;
            object.insert(QStringLiteral("phase"), recordingWriterPhaseName(status.phase()));
            object.insert(QStringLiteral("pendingWriteBytes"), status.pendingWriteBytes());
            object.insert(QStringLiteral("maxPendingWriteBytes"), status.maxPendingWriteBytes());
            object.insert(QStringLiteral("framesCaptured"), status.framesCaptured());
            object.insert(QStringLiteral("framesWritten"), status.framesWritten());
            object.insert(QStringLiteral("droppedFrames"), status.droppedFrames());
            object.insert(QStringLiteral("bytesWritten"), status.bytesWritten());
            object.insert(QStringLiteral("error"), status.errorMessage());
            return object;
        }

        QJsonObject particleMeasurementToJson(
            const scopeone::core::ScopeOneCore::ParticleMeasurement& particle)
        {
            QJsonObject bounds;
            bounds.insert(QStringLiteral("x"), particle.bounds.x());
            bounds.insert(QStringLiteral("y"), particle.bounds.y());
            bounds.insert(QStringLiteral("width"), particle.bounds.width());
            bounds.insert(QStringLiteral("height"), particle.bounds.height());
            QJsonObject centroid;
            centroid.insert(QStringLiteral("x"), particle.centroid.x());
            centroid.insert(QStringLiteral("y"), particle.centroid.y());
            QJsonObject object;
            object.insert(QStringLiteral("area"), particle.area);
            object.insert(QStringLiteral("bounds"), bounds);
            object.insert(QStringLiteral("centroid"), centroid);
            return object;
        }

        // Converts one supported axis name into a recording axis
        bool axisFromName(const QString& name, scopeone::core::RecordingAxis& axis)
        {
            const QString normalized = name.trimmed().toLower();
            if (normalized == QStringLiteral("time"))
            {
                axis = scopeone::core::RecordingAxis::Time;
                return true;
            }
            if (normalized == QStringLiteral("z"))
            {
                axis = scopeone::core::RecordingAxis::Z;
                return true;
            }
            if (normalized == QStringLiteral("xy"))
            {
                axis = scopeone::core::RecordingAxis::XY;
                return true;
            }
            return false;
        }

        // Reads one optional numeric array without coercing invalid values
        bool doubleArrayFromJson(const QJsonValue& value,
                                 const QString& fieldName,
                                 std::vector<double>& values,
                                 QString& errorMessage)
        {
            values.clear();
            if (value.isUndefined())
            {
                return true;
            }
            if (!value.isArray())
            {
                errorMessage = QStringLiteral("%1 must be an array").arg(fieldName);
                return false;
            }

            const QJsonArray array = value.toArray();
            values.reserve(static_cast<size_t>(array.size()));
            for (qsizetype index = 0; index < array.size(); ++index)
            {
                const QJsonValue item = array.at(index);
                if (!item.isDouble() || !std::isfinite(item.toDouble()))
                {
                    errorMessage = QStringLiteral("%1[%2] must be a finite number")
                                       .arg(fieldName)
                                       .arg(index);
                    return false;
                }
                values.push_back(item.toDouble());
            }
            return true;
        }

        // Reads one optional XY array without dropping malformed positions
        bool pointArrayFromJson(const QJsonValue& value,
                                std::vector<QPointF>& points,
                                QString& errorMessage)
        {
            points.clear();
            if (value.isUndefined())
            {
                return true;
            }
            if (!value.isArray())
            {
                errorMessage = QStringLiteral("positions must be an array");
                return false;
            }

            const QJsonArray array = value.toArray();
            points.reserve(static_cast<size_t>(array.size()));
            for (qsizetype index = 0; index < array.size(); ++index)
            {
                const QJsonValue item = array.at(index);
                double x = 0.0;
                double y = 0.0;
                if (item.isArray())
                {
                    const QJsonArray point = item.toArray();
                    if (point.size() != 2 || !point.at(0).isDouble() || !point.at(1).isDouble())
                    {
                        errorMessage = QStringLiteral("positions[%1] must contain exactly two numbers")
                                           .arg(index);
                        return false;
                    }
                    x = point.at(0).toDouble();
                    y = point.at(1).toDouble();
                }
                else if (item.isObject())
                {
                    const QJsonObject point = item.toObject();
                    if (point.size() != 2
                        || !point.value(QStringLiteral("x")).isDouble()
                        || !point.value(QStringLiteral("y")).isDouble())
                    {
                        errorMessage = QStringLiteral("positions[%1] must contain numeric x and y fields")
                                           .arg(index);
                        return false;
                    }
                    x = point.value(QStringLiteral("x")).toDouble();
                    y = point.value(QStringLiteral("y")).toDouble();
                }
                else
                {
                    errorMessage = QStringLiteral("positions[%1] must be an XY array or object").arg(index);
                    return false;
                }
                if (!std::isfinite(x) || !std::isfinite(y))
                {
                    errorMessage = QStringLiteral("positions[%1] must contain finite coordinates").arg(index);
                    return false;
                }
                points.emplace_back(x, y);
            }
            return true;
        }

        bool experimentDocumentFromRequest(const QJsonObject& request,
                                           scopeone::core::ExperimentDocument& document,
                                           QString& errorMessage)
        {
            const QJsonValue value = request.value(QStringLiteral("document"));
            if (!value.isObject())
            {
                errorMessage = QStringLiteral("document must be an object");
                return false;
            }
            return scopeone::core::experimentDocumentFromJson(value.toObject(), document, &errorMessage);
        }

        void copyValidPresentation(const scopeone::core::ExperimentDocument& source,
                                   scopeone::core::ExperimentDocument& destination)
        {
            destination.layers.clear();
            destination.markups.clear();
            QSet<QString> layerIds;
            for (const scopeone::core::DocumentLayer& layer : source.layers)
            {
                if (layer.width > 0 && layer.height > 0)
                {
                    destination.layers.append(layer);
                    layerIds.insert(layer.id);
                }
            }
            for (const scopeone::core::DocumentMarkup& markup : source.markups)
            {
                if (layerIds.contains(markup.layerId))
                {
                    destination.markups.append(markup);
                }
            }
        }

        // Reads a string field or returns the requested default value
        QString stringValueOrDefault(const QJsonObject& object, const QString& key, const QString& defaultValue)
        {
            const QString value = object.value(key).toString().trimmed();
            return value.isEmpty() ? defaultValue : value;
        }

        QStringList stringArrayFromJson(const QJsonArray& array)
        {
            QStringList values;
            values.reserve(array.size());
            for (const QJsonValue& value : array)
            {
                const QString text = value.toString().trimmed();
                if (!text.isEmpty())
                {
                    values.append(text);
                }
            }
            return values;
        }

        QString layerLayoutModeName(PreviewWidget::LayerLayoutMode mode)
        {
            return mode == PreviewWidget::LayerLayoutMode::Overlay
                       ? QStringLiteral("overlay")
                       : QStringLiteral("side_by_side");
        }

        void insertLayerDisplayFields(QJsonObject& object,
                                      const scopeone::core::DocumentLayer& layer,
                                      bool autoStretch)
        {
            object.insert(QStringLiteral("visible"), layer.display.visible);
            object.insert(QStringLiteral("opacityPercent"), layer.display.opacityPercent);
            object.insert(QStringLiteral("gamma"), layer.display.gamma);
            object.insert(QStringLiteral("colormap"), layer.display.colormap);
            object.insert(QStringLiteral("blending"), layer.display.blending);
            object.insert(QStringLiteral("minLevel"), layer.display.levelMin);
            object.insert(QStringLiteral("maxLevel"), layer.display.levelMax);
            object.insert(QStringLiteral("maxPossible"), layer.display.levelDomainMax);
            object.insert(QStringLiteral("autoStretch"), autoStretch);
        }

        QJsonArray layersToJson(const ImageSceneModel* sceneModel,
                                const PreviewWidget* previewWidget)
        {
            QJsonArray layers;
            for (const QString& layerKey : sceneModel->layerIds())
            {
                scopeone::core::DocumentLayer documentLayer;
                if (!sceneModel->findLayer(layerKey, documentLayer))
                {
                    continue;
                }
                QJsonObject layer;
                layer.insert(QStringLiteral("layerKey"), layerKey);
                layer.insert(QStringLiteral("sourceId"), documentLayer.sourceId);
                layer.insert(QStringLiteral("name"), documentLayer.name);
                layer.insert(QStringLiteral("info"), previewWidget->layerInfoText(layerKey));
                insertLayerDisplayFields(
                    layer, documentLayer, sceneModel->layerAutoStretchEnabled(layerKey));
                layer.insert(QStringLiteral("kind"), ImageSceneModel::layerKindName(documentLayer.kind));
                layer.insert(QStringLiteral("width"), documentLayer.width);
                layer.insert(QStringLiteral("height"), documentLayer.height);
                layers.append(layer);
            }
            return layers;
        }

        QJsonArray markupsToJson(const ImageSceneModel* sceneModel)
        {
            QJsonArray markups;
            for (const ImageSceneModel::Markup& markup : sceneModel->markups())
            {
                markups.append(markupToJson(markup));
            }
            return markups;
        }

        QJsonObject histogramToJson(const scopeone::core::ScopeOneCore::HistogramStats& stats)
        {
            QJsonArray bins;
            for (const int count : stats.histogram)
            {
                bins.append(count);
            }
            QJsonObject object;
            object.insert(QStringLiteral("mean"), stats.mean);
            object.insert(QStringLiteral("min"), stats.minVal);
            object.insert(QStringLiteral("max"), stats.maxVal);
            object.insert(QStringLiteral("stdDev"), stats.stdDev);
            object.insert(QStringLiteral("totalPixels"), stats.totalPixels);
            object.insert(QStringLiteral("bitDepth"), stats.bitDepth);
            object.insert(QStringLiteral("maxValue"), stats.maxValue);
            object.insert(QStringLiteral("autoMinLevel"), stats.autoMinLevel);
            object.insert(QStringLiteral("autoMaxLevel"), stats.autoMaxLevel);
            object.insert(QStringLiteral("bins"), bins);
            return object;
        }

        QJsonObject apiCapabilities()
        {
            QJsonObject groups;
            groups.insert(QStringLiteral("system"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("ping"), QStringLiteral("version"), QStringLiteral("status"),
                QStringLiteral("capabilities"), QStringLiteral("state_snapshot"),
                QStringLiteral("frame_mapping_info")
            }));
            groups.insert(QStringLiteral("configuration"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("camera_ids"), QStringLiteral("loaded_devices"),
                QStringLiteral("load_config"), QStringLiteral("unload_config"),
                QStringLiteral("config_groups"), QStringLiteral("configs"),
                QStringLiteral("current_config"), QStringLiteral("set_config")
            }));
            groups.insert(QStringLiteral("preview"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("start_preview"), QStringLiteral("stop_preview"),
                QStringLiteral("list_layers"), QStringLiteral("layer_options"),
                QStringLiteral("set_layer_layout"), QStringLiteral("set_visible_layers"),
                QStringLiteral("set_layer_display"), QStringLiteral("auto_layer_levels"),
                QStringLiteral("full_layer_levels"), QStringLiteral("set_layer_auto_stretch"),
                QStringLiteral("get_source_display_transform"),
                QStringLiteral("set_source_display_transform"),
                QStringLiteral("reset_source_display_transform"), QStringLiteral("move_layer"),
                QStringLiteral("remove_static_layer"), QStringLiteral("clear_static_layers")
            }));
            groups.insert(QStringLiteral("markup"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("create_line_markup"), QStringLiteral("create_rect_markup"),
                QStringLiteral("list_markups"), QStringLiteral("update_markup"),
                QStringLiteral("remove_markup"), QStringLiteral("clear_markups")
            }));
            groups.insert(QStringLiteral("analysis"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("get_layer_histogram"), QStringLiteral("get_pixel_value"),
                QStringLiteral("get_line_profile"), QStringLiteral("detect_particles")
            }));
            groups.insert(QStringLiteral("hardware"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("device_properties"), QStringLiteral("device_property_names"),
                QStringLiteral("get_property"), QStringLiteral("set_property"),
                QStringLiteral("read_exposure"), QStringLiteral("set_exposure"),
                QStringLiteral("get_roi"), QStringLiteral("set_roi"),
                QStringLiteral("set_half_roi"), QStringLiteral("clear_roi"),
                QStringLiteral("xy_stage_devices"), QStringLiteral("z_stage_devices"),
                QStringLiteral("current_xy_stage_device"), QStringLiteral("current_focus_device"),
                QStringLiteral("read_xy_position"), QStringLiteral("read_z_position"),
                QStringLiteral("move_xy_relative"), QStringLiteral("move_z_relative"),
                QStringLiteral("move_xy_to"), QStringLiteral("move_z_to")
            }));
            groups.insert(QStringLiteral("mosaic"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("start_stage_mosaic"), QStringLiteral("stage_mosaic_status"),
                QStringLiteral("cancel_stage_mosaic")
            }));
            groups.insert(QStringLiteral("processing"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("processing_modules"), QStringLiteral("set_processing_bit_depth"),
                QStringLiteral("set_realtime_processing"), QStringLiteral("add_processing_module"),
                QStringLiteral("remove_processing_module"),
                QStringLiteral("set_processing_module_parameters"),
                QStringLiteral("reset_processing_module_state")
            }));
            groups.insert(QStringLiteral("experiment"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("experiment_document"), QStringLiteral("validate_experiment"),
                QStringLiteral("save_experiment"), QStringLiteral("load_experiment"),
                QStringLiteral("start_experiment"), QStringLiteral("experiment_status"),
                QStringLiteral("cancel_experiment")
            }));
            groups.insert(QStringLiteral("recording"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("record"), QStringLiteral("session_info"),
                QStringLiteral("session_close"), QStringLiteral("session_frame"),
                QStringLiteral("latest_raw_frame"), QStringLiteral("session_process_frame"),
                QStringLiteral("session_save")
            }));
            groups.insert(QStringLiteral("frame"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("layer_frame"), QStringLiteral("process_frame_mapping"),
                QStringLiteral("show_frame_mapping_as_layer"),
                QStringLiteral("save_frame_mapping")
            }));

            QJsonObject object;
            object.insert(QStringLiteral("localOnly"), true);
            object.insert(QStringLiteral("requestId"), true);
            object.insert(QStringLiteral("requestMode"), QStringLiteral("sequential"));
            object.insert(QStringLiteral("controlTransport"), QStringLiteral("local_socket"));
            object.insert(QStringLiteral("maxMessageBytes"), static_cast<int>(kMaxMessageBytes));
            object.insert(QStringLiteral("experimentDocumentSchemaVersion"),
                          scopeone::core::kExperimentDocumentSchemaVersion);
            object.insert(QStringLiteral("operationGroups"), groups);
            object.insert(QStringLiteral("longRunningOperations"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("start_experiment"), QStringLiteral("record"),
                QStringLiteral("start_stage_mosaic")
            }));
            object.insert(QStringLiteral("hardwareMutationOperations"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("load_config"), QStringLiteral("unload_config"),
                QStringLiteral("start_preview"), QStringLiteral("stop_preview"),
                QStringLiteral("set_config"), QStringLiteral("set_property"),
                QStringLiteral("set_exposure"), QStringLiteral("set_roi"), QStringLiteral("set_half_roi"),
                QStringLiteral("clear_roi"), QStringLiteral("move_xy_relative"),
                QStringLiteral("move_z_relative"), QStringLiteral("move_xy_to"),
                QStringLiteral("move_z_to"), QStringLiteral("start_experiment"),
                QStringLiteral("cancel_experiment"), QStringLiteral("record"),
                QStringLiteral("start_stage_mosaic"), QStringLiteral("cancel_stage_mosaic")
            }));
            object.insert(QStringLiteral("filesystemMutationOperations"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("save_experiment"), QStringLiteral("start_experiment"),
                QStringLiteral("save_frame_mapping"), QStringLiteral("session_save")
            }));
            object.insert(QStringLiteral("destructiveOperations"), QJsonArray::fromStringList(QStringList{
                QStringLiteral("unload_config"), QStringLiteral("remove_static_layer"),
                QStringLiteral("clear_static_layers"), QStringLiteral("remove_markup"),
                QStringLiteral("clear_markups"), QStringLiteral("remove_processing_module"),
                QStringLiteral("load_experiment"), QStringLiteral("cancel_experiment"),
                QStringLiteral("session_close"), QStringLiteral("cancel_stage_mosaic")
            }));
            object.insert(QStringLiteral("frameTransport"), QStringLiteral("shared_memory"));
            return object;
        }

        bool layerLayoutModeFromName(const QString& name, PreviewWidget::LayerLayoutMode& mode)
        {
            QString normalized = name.trimmed().toLower();
            normalized.remove(QLatin1Char('_'));
            normalized.remove(QLatin1Char('-'));
            normalized.remove(QLatin1Char(' '));
            if (normalized == QStringLiteral("overlay"))
            {
                mode = PreviewWidget::LayerLayoutMode::Overlay;
                return true;
            }
            if (normalized == QStringLiteral("sidebyside"))
            {
                mode = PreviewWidget::LayerLayoutMode::SideBySide;
                return true;
            }
            return false;
        }

        // Reads one required integer field
        bool intField(const QJsonObject& object, const QString& key, int& value)
        {
            const QJsonValue field = object.value(key);
            if (!field.isDouble())
            {
                return false;
            }
            const double number = field.toDouble();
            if (!std::isfinite(number)
                || std::trunc(number) != number
                || number < static_cast<double>((std::numeric_limits<int>::min)())
                || number > static_cast<double>((std::numeric_limits<int>::max)()))
            {
                return false;
            }
            value = static_cast<int>(number);
            return true;
        }

        QString saveRequestError(const QJsonObject& request)
        {
            if (request.value(QStringLiteral("saveDir")).toString().trimmed().isEmpty())
            {
                return QStringLiteral("Missing saveDir");
            }
            if (request.value(QStringLiteral("baseName")).toString().trimmed().isEmpty())
            {
                return QStringLiteral("Missing baseName");
            }
            const QJsonValue formatValue = request.value(QStringLiteral("format"));
            if (!formatValue.isUndefined())
            {
                if (!formatValue.isString())
                {
                    return QStringLiteral("format must be a string");
                }
                const QString formatName = formatValue.toString().trimmed().toLower();
                if (formatName != QStringLiteral("ome-tiff")
                    && formatName != QStringLiteral("ome-zarr")
                    && formatName != QStringLiteral("tiff")
                    && formatName != QStringLiteral("binary"))
                {
                    return QStringLiteral("Unsupported recording format: %1").arg(formatName);
                }
            }
            return {};
        }

        template <typename SaveTarget>
        void applySaveRequest(const QJsonObject& request, SaveTarget& capturePlan)
        {
            capturePlan.saveDir = request.value(QStringLiteral("saveDir")).toString().trimmed();
            capturePlan.baseName = request.value(QStringLiteral("baseName")).toString().trimmed();
            const QString formatName = request.value(QStringLiteral("format"))
                                           .toString(QStringLiteral("ome-tiff"))
                                           .trimmed()
                                           .toLower();
            if (formatName == QStringLiteral("binary"))
            {
                capturePlan.format = scopeone::core::RecordingFormat::Binary;
            }
            else if (formatName == QStringLiteral("tiff"))
            {
                capturePlan.format = scopeone::core::RecordingFormat::Tiff;
            }
            else if (formatName == QStringLiteral("ome-zarr"))
            {
                capturePlan.format = scopeone::core::RecordingFormat::OmeZarr;
            }
            else
            {
                capturePlan.format = scopeone::core::RecordingFormat::OmeTiff;
            }
            capturePlan.enableCompression = request.value(QStringLiteral("compression")).toBool(false);
            capturePlan.compressionLevel = request.value(QStringLiteral("compressionLevel")).toInt(6);
        }

        // Resolves a request camera target against loaded cameras
        QStringList resolveCameraIds(scopeone::core::ScopeOneCore* core, const QString& cameraIdOrAll)
        {
            const QStringList availableCameraIds = core->cameraIds();
            if (availableCameraIds.isEmpty())
            {
                return {};
            }

            const QString target = cameraIdOrAll.trimmed();
            if (target.isEmpty())
            {
                return {};
            }
            if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
            {
                return availableCameraIds;
            }
            if (!availableCameraIds.contains(target))
            {
                return {};
            }
            return QStringList{target};
        }

        // Parses the convenience recording request without starting acquisition
        bool recordingPlanFromRequest(scopeone::core::ScopeOneCore* core,
                                      const QJsonObject& request,
                                      scopeone::core::ExperimentPlan& plan,
                                      int& timeoutMs,
                                      QString& errorMessage)
        {
            const QJsonValue framesValue = request.value(QStringLiteral("frames"));
            if (!framesValue.isDouble()
                || std::trunc(framesValue.toDouble()) != framesValue.toDouble()
                || framesValue.toDouble() < static_cast<double>((std::numeric_limits<int>::min)())
                || framesValue.toDouble() > static_cast<double>((std::numeric_limits<int>::max)()))
            {
                errorMessage = QStringLiteral("frames must be an integer");
                return false;
            }
            const QJsonValue cameraValue = request.value(QStringLiteral("camera"));
            if (!cameraValue.isUndefined() && !cameraValue.isString())
            {
                errorMessage = QStringLiteral("camera must be a string");
                return false;
            }

            plan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            plan.cameraIds = resolveCameraIds(
                core,
                cameraValue.isUndefined() ? QStringLiteral("All") : cameraValue.toString());
            plan.format = scopeone::core::RecordingFormat::OmeTiff;
            plan.streamToDisk = false;
            plan.framesPerBurst = framesValue.toInt();
            plan.burstMode = false;
            plan.targetBursts = 1;
            plan.enableCompression = false;
            plan.processing = core->processingRecipe();
            timeoutMs = request.value(QStringLiteral("timeoutMs")).toInt(120000);

            const QJsonValue intervalValue = request.value(QStringLiteral("mdaIntervalMs"));
            if (!intervalValue.isUndefined()
                && (!intervalValue.isDouble() || !std::isfinite(intervalValue.toDouble())))
            {
                errorMessage = QStringLiteral("mdaIntervalMs must be a finite number");
                return false;
            }
            plan.mdaIntervalMs = intervalValue.isUndefined() ? 0.0 : intervalValue.toDouble();
            if (!doubleArrayFromJson(request.value(QStringLiteral("zPositions")),
                                     QStringLiteral("zPositions"),
                                     plan.zPositions,
                                     errorMessage)
                || !pointArrayFromJson(request.value(QStringLiteral("positions")),
                                       plan.positions,
                                       errorMessage))
            {
                return false;
            }

            const QJsonValue orderValue = request.value(QStringLiteral("order"));
            if (orderValue.isUndefined())
            {
                return true;
            }
            if (!orderValue.isArray())
            {
                errorMessage = QStringLiteral("order must be an array");
                return false;
            }
            const QJsonArray orderArray = orderValue.toArray();
            plan.order.clear();
            for (qsizetype index = 0; index < orderArray.size(); ++index)
            {
                scopeone::core::RecordingAxis axis;
                if (!orderArray.at(index).isString()
                    || !axisFromName(orderArray.at(index).toString(), axis))
                {
                    errorMessage = QStringLiteral("order[%1] contains an unsupported axis").arg(index);
                    return false;
                }
                plan.order.push_back(axis);
            }
            return true;
        }

        // Add common frame metadata to a local API response
        void addFrameMetadata(QJsonObject& response, const scopeone::core::ImageFrame& frame)
        {
            response.insert(QStringLiteral("camera"), frame.cameraId);
            response.insert(QStringLiteral("width"), frame.width);
            response.insert(QStringLiteral("height"), frame.height);
            response.insert(QStringLiteral("stride"), frame.stride);
            response.insert(QStringLiteral("payloadBytes"), QString::number(frame.payloadByteCount()));
            response.insert(QStringLiteral("bitsPerSample"), frame.bitsPerSample);
            response.insert(QStringLiteral("pixelFormat"),
                            frame.isMono16()
                                ? QStringLiteral("Mono16")
                                : QStringLiteral("Mono8"));
            response.insert(QStringLiteral("frameIndex"), QString::number(frame.frameIndex));
            response.insert(QStringLiteral("timestampNs"), QString::number(frame.timestampNs));
            response.insert(QStringLiteral("sourceRoiX"), frame.sourceRoiX);
            response.insert(QStringLiteral("sourceRoiY"), frame.sourceRoiY);
            response.insert(QStringLiteral("sourceRoiWidth"), frame.sourceRoiWidth);
            response.insert(QStringLiteral("sourceRoiHeight"), frame.sourceRoiHeight);
            response.insert(QStringLiteral("sourceRoiValid"), frame.hasSourceRoi());
        }

        struct ProcessFrameRequestResult
        {
            scopeone::core::ImageFrame frame;
            int moduleIndex{-1};
            int nextModuleIndex{-1};
            int startModuleIndex{-1};
        };

        struct AsyncProcessResult
        {
            ProcessFrameRequestResult processed;
            QString errorMessage;
        };

        // Process one frame according to optional local API stage fields
        ProcessFrameRequestResult processFrameFromRequest(scopeone::core::ScopeOneCore* core,
                                                          const scopeone::core::ImageFrame& frame,
                                                          const QJsonObject& request,
                                                          QString& errorMessage)
        {
            const bool hasStart = request.contains(QStringLiteral("startModuleIndex"));
            const bool hasEnd = request.contains(QStringLiteral("endModuleIndex"));
            if (hasStart && hasEnd)
            {
                errorMessage = QStringLiteral("Use either startModuleIndex or endModuleIndex");
                return {};
            }

            const int moduleCount = static_cast<int>(core->processingModules().size());
            ProcessFrameRequestResult result;
            if (hasEnd)
            {
                const int endModuleIndex = request.value(QStringLiteral("endModuleIndex")).toInt(-1);
                if (endModuleIndex < 0 || endModuleIndex >= moduleCount)
                {
                    errorMessage = QStringLiteral("endModuleIndex is outside the processing pipeline");
                    return {};
                }
                result.frame = core->processFrameThrough(endModuleIndex, frame);
                result.moduleIndex = endModuleIndex;
                result.nextModuleIndex = endModuleIndex + 1;
                return result;
            }
            if (hasStart)
            {
                const int startModuleIndex = request.value(QStringLiteral("startModuleIndex")).toInt(-1);
                if (startModuleIndex < 0 || startModuleIndex > moduleCount)
                {
                    errorMessage = QStringLiteral("startModuleIndex is outside the processing pipeline");
                    return {};
                }
                result.frame = core->processFrameFrom(startModuleIndex, frame);
                result.startModuleIndex = startModuleIndex;
                return result;
            }
            result.frame = core->processFrame(frame);
            return result;
        }

        // Add actual processing stage fields to a local API response
        void addProcessingMetadata(QJsonObject& response, const ProcessFrameRequestResult& result)
        {
            if (result.moduleIndex >= 0)
            {
                response.insert(QStringLiteral("moduleIndex"), result.moduleIndex);
                response.insert(QStringLiteral("nextModuleIndex"), result.nextModuleIndex);
            }
            if (result.startModuleIndex >= 0)
            {
                response.insert(QStringLiteral("startModuleIndex"), result.startModuleIndex);
            }
        }

        QJsonArray savedFramePaths(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            QJsonArray paths;
            const auto& files = session->outputFiles();
            for (const QString& cameraId : session->recordedCameraIds())
            {
                const auto it = files.constFind(cameraId);
                if (it != files.constEnd() && !it.value().rawPath.isEmpty())
                {
                    paths.append(it.value().rawPath);
                }
            }
            return paths;
        }

        scopeone::core::ImageFrame publishExternalApiFrame(
            scopeone::core::ScopeOneCore* core,
            const scopeone::core::ImageFrame& frame,
            QString& errorMessage)
        {
            const QString sourceId = frame.cameraId.trimmed().isEmpty()
                                         ? QStringLiteral("frame_mapping")
                                         : frame.cameraId.trimmed();
            scopeone::core::ImageFrame graphFrame = core->publishExternalFrame(sourceId, frame);
            if (!graphFrame.isValid())
            {
                errorMessage = QStringLiteral("Failed to publish external frame");
            }
            return graphFrame;
        }

    } // namespace

    // Starts the local API pipe server and frame mapping
    ScopeOneLocalApiServer::ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                                   PreviewWidget* previewWidget,
                                                   QObject* parent)
        : QObject(parent)
          , m_scopeonecore(core)
          , m_previewWidget(previewWidget)
          , m_sceneModel(core ? core->imageSceneModel() : nullptr)
          , m_server(new QLocalServer(this))
    {
        if (!core)
        {
            qFatal("ScopeOneLocalApiServer requires ScopeOneCore");
        }
        m_taskPool.setMaxThreadCount(1);

        QLocalServer::removeServer(kServerName);
        connect(m_server, &QLocalServer::newConnection,
                this, &ScopeOneLocalApiServer::handleNewConnection);

        if (!m_server->listen(kServerName))
        {
            qWarning().noquote()
                << QStringLiteral(
                    "ScopeOne local API server failed to listen on '%1': %2. Another ScopeOne instance may already be running.")
                .arg(kServerName, m_server->errorString());
        }
        else
        {
            qInfo().noquote()
                << QStringLiteral("ScopeOne local API server listening on '%1'")
                .arg(m_server->fullServerName());
        }

#if defined(_WIN32)
        const DWORD mappingSize = static_cast<DWORD>(
            scopeone::core::kSharedFrameHeaderSize + scopeone::core::kSharedFrameMaxBytes);
        m_frameMappingHandle = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                                  nullptr,
                                                  PAGE_READWRITE,
                                                  0,
                                                  mappingSize,
                                                  reinterpret_cast<LPCWSTR>(kFrameMappingName.utf16()));
        if (!m_frameMappingHandle)
        {
            qWarning().noquote() << QStringLiteral("ScopeOne API frame mapping create failed");
            return;
        }
        m_frameMappingView = static_cast<uchar*>(
            MapViewOfFile(m_frameMappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, mappingSize));
        if (!m_frameMappingView)
        {
            qWarning().noquote() << QStringLiteral("ScopeOne API frame mapping view failed");
            CloseHandle(m_frameMappingHandle);
            m_frameMappingHandle = nullptr;
        }
#else
        const size_t mappingSize = static_cast<size_t>(
            scopeone::core::kSharedFrameHeaderSize + scopeone::core::kSharedFrameMaxBytes);
        m_frameShmFd = ::shm_open(kPosixFrameShmName, O_CREAT | O_RDWR, 0600);
        if (m_frameShmFd < 0)
        {
            qWarning().noquote() << QStringLiteral("ScopeOne API frame shm create failed");
            return;
        }
        if (::ftruncate(m_frameShmFd, static_cast<off_t>(mappingSize)) != 0)
        {
            qWarning().noquote() << QStringLiteral("ScopeOne API frame shm resize failed");
            ::close(m_frameShmFd);
            m_frameShmFd = -1;
            ::shm_unlink(kPosixFrameShmName);
            return;
        }
        void* view = ::mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_frameShmFd, 0);
        if (view == MAP_FAILED)
        {
            qWarning().noquote() << QStringLiteral("ScopeOne API frame shm map failed");
            ::close(m_frameShmFd);
            m_frameShmFd = -1;
            ::shm_unlink(kPosixFrameShmName);
            return;
        }
        m_frameMappingView = static_cast<uchar*>(view);
#endif
    }

    // Releases local API shared frame resources
    ScopeOneLocalApiServer::~ScopeOneLocalApiServer()
    {
        m_taskPool.waitForDone();
#if defined(_WIN32)
        if (m_frameMappingView)
        {
            UnmapViewOfFile(m_frameMappingView);
            m_frameMappingView = nullptr;
        }
        if (m_frameMappingHandle)
        {
            CloseHandle(m_frameMappingHandle);
            m_frameMappingHandle = nullptr;
        }
#else
        if (m_frameMappingView)
        {
            const size_t mappingSize = static_cast<size_t>(
                scopeone::core::kSharedFrameHeaderSize + scopeone::core::kSharedFrameMaxBytes);
            ::munmap(m_frameMappingView, mappingSize);
            m_frameMappingView = nullptr;
        }
        if (m_frameShmFd >= 0)
        {
            ::close(m_frameShmFd);
            m_frameShmFd = -1;
            ::shm_unlink(kPosixFrameShmName);
        }
#endif
    }

    // Accepts pending local API socket connections
    void ScopeOneLocalApiServer::handleNewConnection()
    {
        while (QLocalSocket* socket = m_server->nextPendingConnection())
        {
            connect(socket, &QLocalSocket::readyRead,
                    this, [this, socket]() { handleSocketReadyRead(socket); });
            connect(socket, &QLocalSocket::disconnected,
                    this, [this, socket]() { handleSocketDisconnected(socket); });
        }
    }

    // Reads and dispatches framed JSON messages from one socket
    void ScopeOneLocalApiServer::handleSocketReadyRead(QLocalSocket* socket)
    {
        QByteArray& buffer = m_readBuffers[socket];
        buffer += socket->readAll();

        while (true)
        {
            QJsonObject request;
            const DecodeResult result = tryDecodeMessage(buffer, request);
            if (result == DecodeResult::Incomplete)
            {
                return;
            }
            if (result == DecodeResult::Error)
            {
                sendResponse(socket, makeResponse(QStringLiteral("error"), false));
                socket->disconnectFromServer();
                return;
            }

            const QJsonValue requestId = request.value(QStringLiteral("requestId"));
            const QString requestType = request.value(QStringLiteral("type")).toString().trimmed();
            QJsonObject response;
            if (!requestId.isUndefined() && !requestId.isString() && !requestId.isDouble())
            {
                response = makeResponse(requestType, false);
                response.insert(QStringLiteral("error"), QStringLiteral("requestId must be a string or number"));
            }
            else if (m_scopeonecore->configurationOperationRunning()
                     && requestType != QStringLiteral("ping")
                     && requestType != QStringLiteral("version")
                     && requestType != QStringLiteral("capabilities")
                     && requestType != QStringLiteral("status")
                     && requestType != QStringLiteral("state_snapshot")
                     && requestType != QStringLiteral("frame_mapping_info"))
            {
                response = makeResponse(requestType, false);
                response.insert(QStringLiteral("error"),
                                QStringLiteral("A configuration operation is still running"));
            }
            else
            {
                if (processAsyncRequest(socket, request, requestId))
                {
                    continue;
                }
                response = processRequest(request);
            }
            sendRequestResponse(socket, response, requestId);
        }
    }

    // Cleans up socket state after disconnect
    void ScopeOneLocalApiServer::handleSocketDisconnected(QLocalSocket* socket)
    {
        m_readBuffers.remove(socket);
        socket->deleteLater();
    }

    // Writes one framed JSON response to a socket
    void ScopeOneLocalApiServer::sendResponse(QLocalSocket* socket, const QJsonObject& response)
    {
        QByteArray message = encodeMessage(response);
        if (message.isEmpty())
        {
            QJsonObject errorResponse;
            errorResponse.insert(QStringLiteral("type"), QStringLiteral("error"));
            errorResponse.insert(QStringLiteral("ok"), false);
            errorResponse.insert(QStringLiteral("error"), QStringLiteral("Control response exceeds 64 MiB"));
            if (response.contains(QStringLiteral("requestId")))
            {
                errorResponse.insert(QStringLiteral("requestId"), response.value(QStringLiteral("requestId")));
            }
            message = encodeMessage(errorResponse);
        }
        socket->write(message);
        socket->flush();
    }

    // Restores request correlation before sending a deferred response
    void ScopeOneLocalApiServer::sendRequestResponse(QLocalSocket* socket,
                                                     QJsonObject response,
                                                     const QJsonValue& requestId)
    {
        if (!socket)
        {
            return;
        }
        if (!requestId.isUndefined())
        {
            response.insert(QStringLiteral("requestId"), requestId);
        }
        sendResponse(socket, response);
    }

    // Starts Local API operations that must not run in the socket callback
    bool ScopeOneLocalApiServer::processAsyncRequest(QLocalSocket* socket,
                                                     const QJsonObject& request,
                                                     const QJsonValue& requestId)
    {
        const QString type = request.value(QStringLiteral("type")).toString().trimmed();
        const QPointer<QLocalSocket> guardedSocket(socket);
        const auto finish = [this, guardedSocket, requestId](QJsonObject response)
        {
            if (guardedSocket)
            {
                sendRequestResponse(guardedSocket, std::move(response), requestId);
            }
        };

        if (type == QStringLiteral("load_config"))
        {
            const QString configPath = request.value(QStringLiteral("configPath")).toString().trimmed();
            if (configPath.isEmpty() || !m_scopeonecore->loadConfiguration(configPath))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                configPath.isEmpty()
                                    ? QStringLiteral("Missing configPath")
                                    : m_scopeonecore->configurationError());
                response.insert(QStringLiteral("state"), m_scopeonecore->configurationState());
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::configurationLoadFinished,
                    context,
                    [this, context, finish, type](
                        bool success,
                        const scopeone::core::ScopeOneCore::LoadConfigResult& result,
                        const QString& errorMessage)
                    {
                        QJsonObject response = makeResponse(type, success);
                        response.insert(QStringLiteral("state"), m_scopeonecore->configurationState());
                        response.insert(QStringLiteral("complete"),
                                        m_scopeonecore->configurationState()
                                            == QStringLiteral("loaded"));
                        response.insert(QStringLiteral("cameraIds"),
                                        QJsonArray::fromStringList(result.cameraIds));
                        response.insert(QStringLiteral("failedDevices"),
                                        QJsonArray::fromStringList(result.failedDevices));
                        response.insert(QStringLiteral("successCount"), result.successCount);
                        response.insert(QStringLiteral("failCount"), result.failCount);
                        response.insert(QStringLiteral("skippedCameraCount"),
                                        result.skippedCameraCount);
                        if (!success)
                        {
                            response.insert(QStringLiteral("error"),
                                            errorMessage.isEmpty()
                                                ? QStringLiteral("Failed to load configuration")
                                                : errorMessage);
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            return true;
        }

        if (type == QStringLiteral("unload_config"))
        {
            if (!m_scopeonecore->unloadConfiguration())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), m_scopeonecore->configurationError());
                response.insert(QStringLiteral("state"), m_scopeonecore->configurationState());
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::configurationUnloadFinished,
                    context, [this, context, finish, type](bool success, const QString& errorMessage)
                    {
                        QJsonObject response = makeResponse(type, success);
                        response.insert(QStringLiteral("state"), m_scopeonecore->configurationState());
                        response.insert(QStringLiteral("complete"),
                                        m_scopeonecore->configurationState()
                                            == QStringLiteral("unloaded"));
                        if (!success)
                        {
                            response.insert(QStringLiteral("error"),
                                            errorMessage.isEmpty()
                                                ? QStringLiteral("Failed to unload configuration")
                                                : errorMessage);
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            return true;
        }

        const bool xyRelative = type == QStringLiteral("move_xy_relative");
        const bool zRelative = type == QStringLiteral("move_z_relative");
        const bool xyAbsolute = type == QStringLiteral("move_xy_to");
        const bool zAbsolute = type == QStringLiteral("move_z_to");
        if (xyRelative || zRelative || xyAbsolute || zAbsolute)
        {
            const QString device = stringValueOrDefault(
                request,
                QStringLiteral("device"),
                (xyRelative || xyAbsolute)
                    ? m_scopeonecore->currentXYStageDevice()
                    : m_scopeonecore->currentFocusDevice());
            quint64 commandId = 0;
            if (xyRelative)
            {
                commandId = m_scopeonecore->moveXYRelative(
                    device,
                    request.value(QStringLiteral("dx")).toDouble(),
                    request.value(QStringLiteral("dy")).toDouble());
            }
            else if (zRelative)
            {
                commandId = m_scopeonecore->moveZRelative(
                    device, request.value(QStringLiteral("dz")).toDouble());
            }
            else if (xyAbsolute)
            {
                commandId = m_scopeonecore->moveXYTo(
                    device,
                    request.value(QStringLiteral("x")).toDouble(),
                    request.value(QStringLiteral("y")).toDouble());
            }
            else
            {
                commandId = m_scopeonecore->moveZTo(
                    device, request.value(QStringLiteral("z")).toDouble());
            }
            if (commandId == 0)
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to queue stage move"));
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::stageMoveFinished,
                    context,
                    [context, finish, type, commandId](quint64 completedId,
                                                       const QString&,
                                                       bool success,
                                                       const QString& errorMessage)
                    {
                        if (completedId != commandId)
                        {
                            return;
                        }
                        QJsonObject response = makeResponse(type, success);
                        if (!success)
                        {
                            response.insert(QStringLiteral("error"),
                                            errorMessage.isEmpty()
                                                ? QStringLiteral("Stage move failed")
                                                : errorMessage);
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            return true;
        }

        if (type == QStringLiteral("detect_particles"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            int threshold = 0;
            int minArea = 0;
            int maxArea = 0;
            int maxParticles = 1000;
            const QJsonValue exportMaskValue = request.value(QStringLiteral("exportMask"));
            const QJsonValue publishMaskValue = request.value(QStringLiteral("publishMask"));
            const scopeone::core::ImageFrame frame = m_scopeonecore->graphFrame(layerKey);
            if (layerKey.isEmpty()
                || !intField(request, QStringLiteral("threshold"), threshold)
                || !intField(request, QStringLiteral("minArea"), minArea)
                || !intField(request, QStringLiteral("maxArea"), maxArea)
                || (request.contains(QStringLiteral("maxParticles"))
                    && !intField(request, QStringLiteral("maxParticles"), maxParticles))
                || threshold < 0
                || minArea <= 0
                || maxArea < minArea
                || maxParticles <= 0
                || maxParticles > 10000
                || (!exportMaskValue.isUndefined() && !exportMaskValue.isBool())
                || (!publishMaskValue.isUndefined() && !publishMaskValue.isBool())
                || !frame.isValid()
                || (!frame.isMono8() && !frame.isMono16()))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Invalid particle detection request"));
                finish(std::move(response));
                return true;
            }
            threshold = qMin(threshold, frame.maxValue());
            const quint64 analysisId = m_scopeonecore->detectParticles(
                layerKey, threshold, minArea, maxArea, maxParticles);
            if (analysisId == 0)
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to queue particle detection"));
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::particleDetectionFinished,
                    context,
                    [this, context, finish, type, analysisId, threshold, minArea, maxArea,
                     publishMask = publishMaskValue.toBool(false),
                     exportMask = exportMaskValue.toBool(false)](
                        quint64 completedId,
                        const QString& resultLayerKey,
                        const scopeone::core::ScopeOneCore::ParticleDetectionResult& result,
                        const QString& errorMessage)
                    {
                        if (completedId != analysisId)
                        {
                            return;
                        }
                        if (!errorMessage.isEmpty())
                        {
                            QJsonObject response = makeResponse(type, false);
                            response.insert(QStringLiteral("error"), errorMessage);
                            finish(std::move(response));
                            context->deleteLater();
                            return;
                        }

                        QJsonArray particles;
                        for (const auto& particle : result.particles)
                        {
                            particles.append(particleMeasurementToJson(particle));
                        }
                        QJsonObject response = makeResponse(type, true);
                        response.insert(QStringLiteral("layerKey"), resultLayerKey);
                        response.insert(QStringLiteral("threshold"), threshold);
                        response.insert(QStringLiteral("minArea"), minArea);
                        response.insert(QStringLiteral("maxArea"), maxArea);
                        response.insert(QStringLiteral("particleCount"), particles.size());
                        response.insert(QStringLiteral("truncated"), result.truncated);
                        response.insert(QStringLiteral("particles"), particles);

                        if (publishMask)
                        {
                            const QString maskLayerId = QStringLiteral("particle_mask");
                            const auto publishedMask = m_scopeonecore->publishStaticFrame(
                                maskLayerId, result.mask, QStringLiteral("Particle Mask"));
                            if (!publishedMask.isValid())
                            {
                                response = makeResponse(type, false);
                                response.insert(QStringLiteral("error"),
                                                QStringLiteral("Failed to publish particle mask"));
                            }
                            else
                            {
                                const QString maskLayerKey =
                                    scopeone::core::ScopeOneCore::staticLayerKey(maskLayerId);
                                m_sceneModel->setLayerColormap(maskLayerKey, QStringLiteral("Magenta"));
                                m_sceneModel->setLayerOpacityPercent(maskLayerKey, 70);
                                m_sceneModel->setLayerBlending(maskLayerKey, QStringLiteral("Additive"));
                                QStringList visibleLayers = m_sceneModel->visibleLayerIds();
                                if (!visibleLayers.contains(resultLayerKey)) visibleLayers.append(resultLayerKey);
                                if (!visibleLayers.contains(maskLayerKey)) visibleLayers.append(maskLayerKey);
                                m_sceneModel->setVisibleLayers(visibleLayers);
                                m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
                                response.insert(QStringLiteral("maskLayerKey"), maskLayerKey);
                            }
                        }
                        if (response.value(QStringLiteral("ok")).toBool() && exportMask)
                        {
                            QString exportError;
                            scopeone::core::ImageFrame exportedMask;
                            if (!exportFrameToSharedMemory(result.mask, exportedMask, exportError))
                            {
                                response = makeResponse(type, false);
                                response.insert(QStringLiteral("error"),
                                                exportError.isEmpty()
                                                    ? QStringLiteral("Failed to export particle mask")
                                                    : exportError);
                            }
                            else
                            {
                                QJsonObject mask;
                                mask.insert(QStringLiteral("mappingName"), kFrameMappingName);
                                mask.insert(QStringLiteral("mappingSize"),
                                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                                + scopeone::core::kSharedFrameMaxBytes));
                                addFrameMetadata(mask, exportedMask);
                                response.insert(QStringLiteral("mask"), mask);
                            }
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            return true;
        }

        if (type == QStringLiteral("session_process_frame")
            || type == QStringLiteral("process_frame_mapping"))
        {
            const auto startProcessing = [this, finish, type, request](
                const scopeone::core::ImageFrame& inputFrame)
            {
                auto* watcher = new QFutureWatcher<AsyncProcessResult>(this);
                connect(watcher, &QFutureWatcher<AsyncProcessResult>::finished,
                        this, [this, watcher, finish, type]()
                {
                    AsyncProcessResult task = watcher->result();
                    QJsonObject response = makeResponse(type, false);
                    if (task.processed.frame.isValid())
                    {
                        task.processed.frame = publishExternalApiFrame(
                            m_scopeonecore, task.processed.frame, task.errorMessage);
                    }
                    scopeone::core::ImageFrame exportedFrame;
                    if (!task.processed.frame.isValid()
                        || !exportFrameToSharedMemory(
                            task.processed.frame, exportedFrame, task.errorMessage))
                    {
                        response.insert(QStringLiteral("error"),
                                        task.errorMessage.isEmpty()
                                            ? QStringLiteral("Failed to process frame")
                                            : task.errorMessage);
                    }
                    else
                    {
                        response = makeResponse(type, true);
                        response.insert(QStringLiteral("mappingName"), kFrameMappingName);
                        response.insert(QStringLiteral("mappingSize"),
                                        static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                            + scopeone::core::kSharedFrameMaxBytes));
                        addFrameMetadata(response, exportedFrame);
                        addProcessingMetadata(response, task.processed);
                    }
                    finish(std::move(response));
                    watcher->deleteLater();
                });
                watcher->setFuture(QtConcurrent::run(
                    &m_taskPool,
                    [this, inputFrame, request]()
                    {
                        AsyncProcessResult task;
                        task.processed = processFrameFromRequest(
                            m_scopeonecore, inputFrame, request, task.errorMessage);
                        return task;
                    }));
            };

            if (type == QStringLiteral("process_frame_mapping"))
            {
                scopeone::core::ImageFrame frame;
                QString errorMessage;
                const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
                if (!importFrameFromSharedMemory(cameraId, frame, errorMessage))
                {
                    QJsonObject response = makeResponse(type, false);
                    response.insert(QStringLiteral("error"),
                                    errorMessage.isEmpty()
                                        ? QStringLiteral("Failed to import frame")
                                        : errorMessage);
                    finish(std::move(response));
                    return true;
                }
                frame = publishExternalApiFrame(m_scopeonecore, frame, errorMessage);
                if (!frame.isValid())
                {
                    QJsonObject response = makeResponse(type, false);
                    response.insert(QStringLiteral("error"), errorMessage);
                    finish(std::move(response));
                    return true;
                }
                startProcessing(frame);
                return true;
            }

            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            const auto session = m_scopeonecore->recordingSession(sessionId);
            const quint64 frameRequestId = m_scopeonecore->requestRecordingSessionFrame(
                session, cameraId, index);
            if (frameRequestId == 0)
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                session ? QStringLiteral("Invalid frame request")
                                        : QStringLiteral("Unknown session"));
                finish(std::move(response));
                return true;
            }
            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionFrameReady,
                    context,
                    [context, frameRequestId, startProcessing, finish, type](
                        quint64 completedId,
                        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>&,
                        const QString&,
                        int,
                        const scopeone::core::ImageFrame& frame)
                    {
                        if (completedId != frameRequestId)
                        {
                            return;
                        }
                        if (frame.isValid())
                        {
                            startProcessing(frame);
                        }
                        else
                        {
                            QJsonObject response = makeResponse(type, false);
                            response.insert(QStringLiteral("error"),
                                            QStringLiteral("Frame index out of range"));
                            finish(std::move(response));
                        }
                        context->deleteLater();
                    });
            return true;
        }

        if (type == QStringLiteral("session_frame"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            const auto session = m_scopeonecore->recordingSession(sessionId);
            const quint64 frameRequestId = m_scopeonecore->requestRecordingSessionFrame(
                session, cameraId, index);
            if (frameRequestId == 0)
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                session ? QStringLiteral("Invalid frame request")
                                        : QStringLiteral("Unknown session"));
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionFrameReady,
                    context,
                    [this, context, finish, type, frameRequestId](
                        quint64 completedId,
                        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>&,
                        const QString&,
                        int,
                        const scopeone::core::ImageFrame& frame)
                    {
                        if (completedId != frameRequestId)
                        {
                            return;
                        }
                        QJsonObject response = makeResponse(type, false);
                        QString errorMessage;
                        const auto graphFrame = publishExternalApiFrame(
                            m_scopeonecore, frame, errorMessage);
                        scopeone::core::ImageFrame exportedFrame;
                        if (!graphFrame.isValid()
                            || !exportFrameToSharedMemory(graphFrame, exportedFrame, errorMessage))
                        {
                            response.insert(QStringLiteral("error"),
                                            errorMessage.isEmpty()
                                                ? QStringLiteral("Frame index out of range")
                                                : errorMessage);
                        }
                        else
                        {
                            response = makeResponse(type, true);
                            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
                            response.insert(QStringLiteral("mappingSize"),
                                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                                + scopeone::core::kSharedFrameMaxBytes));
                            addFrameMetadata(response, exportedFrame);
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            return true;
        }

        if (type == QStringLiteral("record"))
        {
            scopeone::core::ExperimentPlan plan;
            int timeoutMs = 120000;
            QString errorMessage;
            if (!recordingPlanFromRequest(
                    m_scopeonecore, request, plan, timeoutMs, errorMessage))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), errorMessage);
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            const auto completed = std::make_shared<bool>(false);
            auto* timeoutTimer = new QTimer(context);
            timeoutTimer->setSingleShot(true);
            const int waitTimeoutMs = timeoutMs > 0 ? timeoutMs : 120000;
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped,
                    context,
                    [context, timeoutTimer, finish, type, completed,
                     experimentId = plan.experimentId](
                        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                    {
                        if (!session || session->capturePlan().experimentId != experimentId)
                        {
                            return;
                        }
                        if (*completed)
                        {
                            return;
                        }
                        *completed = true;
                        timeoutTimer->stop();
                        QJsonObject response = makeResponse(type, session->hasRecordedOutput());
                        if (!session->hasRecordedOutput())
                        {
                            response.insert(
                                QStringLiteral("error"),
                                session->saveMessage().isEmpty()
                                    ? QStringLiteral("Recording finished but captured no frames")
                                    : session->saveMessage());
                        }
                        else if (session->streamedToDisk() && !session->isSaved())
                        {
                            response = makeResponse(type, false);
                            response.insert(QStringLiteral("error"),
                                            session->saveMessage().isEmpty()
                                                ? QStringLiteral("Recording writer failed")
                                                : session->saveMessage());
                        }
                        else
                        {
                            response.insert(QStringLiteral("sessionId"), experimentId);
                            response.insert(QStringLiteral("cameraIds"),
                                            QJsonArray::fromStringList(session->recordedCameraIds()));
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            connect(timeoutTimer, &QTimer::timeout, context,
                    [this, context, finish, type, completed,
                     experimentId = plan.experimentId, waitTimeoutMs]()
                    {
                        if (*completed)
                        {
                            return;
                        }
                        *completed = true;
                        QString ignoredError;
                        m_scopeonecore->cancelExperiment(experimentId, &ignoredError);
                        QJsonObject response = makeResponse(type, false);
                        response.insert(
                            QStringLiteral("error"),
                            QStringLiteral("Recording timed out after %1 ms").arg(waitTimeoutMs));
                        finish(std::move(response));
                        context->deleteLater();
                    });

            scopeone::core::ExperimentDocument document;
            document.plan = plan;
            copyValidPresentation(m_sceneModel->document(), document);
            if (!m_scopeonecore->startExperiment(document, &errorMessage))
            {
                if (!*completed)
                {
                    *completed = true;
                    QJsonObject response = makeResponse(type, false);
                    response.insert(QStringLiteral("error"), errorMessage);
                    finish(std::move(response));
                    context->deleteLater();
                }
                return true;
            }
            if (!*completed)
            {
                timeoutTimer->start(waitTimeoutMs);
            }
            return true;
        }

        if (type == QStringLiteral("session_save"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const auto session = m_scopeonecore->recordingSession(sessionId);
            const QString saveError = saveRequestError(request);
            if (!session || !saveError.isEmpty())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                session ? saveError : QStringLiteral("Unknown session"));
                finish(std::move(response));
                return true;
            }

            const scopeone::core::ExperimentDocument& presentation = m_sceneModel->document();
            if (presentation.plan.experimentId == sessionId)
            {
                QString presentationError;
                if (!m_scopeonecore->setRecordingSessionPresentation(
                        session, presentation, &presentationError))
                {
                    QJsonObject response = makeResponse(type, false);
                    response.insert(QStringLiteral("error"), presentationError);
                    finish(std::move(response));
                    return true;
                }
            }

            scopeone::core::ScopeOneCore::RecordingSaveOptions saveOptions;
            applySaveRequest(request, saveOptions);
            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionSaveFinished,
                    context,
                    [context, finish, type, session](
                        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& completed)
                    {
                        if (completed != session)
                        {
                            return;
                        }
                        QJsonObject response = makeResponse(type, session->isSaved());
                        if (session->isSaved())
                        {
                            response.insert(QStringLiteral("paths"), savedFramePaths(session));
                        }
                        else
                        {
                            response.insert(QStringLiteral("error"), session->saveMessage());
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            if (!m_scopeonecore->saveRecordingSession(session, saveOptions))
            {
                delete context;
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Session save is already in progress"));
                finish(std::move(response));
            }
            return true;
        }

        if (type == QStringLiteral("save_frame_mapping"))
        {
            const QString saveError = saveRequestError(request);
            scopeone::core::ImageFrame frame;
            QString errorMessage;
            if (!saveError.isEmpty()
                || !importFrameFromSharedMemory(
                    request.value(QStringLiteral("camera")).toString().trimmed(),
                    frame,
                    errorMessage))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                !saveError.isEmpty() ? saveError : errorMessage);
                finish(std::move(response));
                return true;
            }
            frame = publishExternalApiFrame(m_scopeonecore, frame, errorMessage);
            if (!frame.isValid())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), errorMessage);
                finish(std::move(response));
                return true;
            }

            scopeone::core::ExperimentPlan capturePlan;
            capturePlan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            capturePlan.cameraIds.append(frame.cameraId);
            capturePlan.streamToDisk = false;
            capturePlan.processing = m_scopeonecore->processingRecipe();
            applySaveRequest(request, capturePlan);
            const auto session = m_scopeonecore->createFrameSession({frame}, capturePlan);
            if (!session)
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to create frame session"));
                finish(std::move(response));
                return true;
            }

            auto* context = new QObject(this);
            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionSaveFinished,
                    context,
                    [context, finish, type, session, frame](
                        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& completed)
                    {
                        if (completed != session)
                        {
                            return;
                        }
                        QJsonObject response = makeResponse(type, session->isSaved());
                        if (session->isSaved())
                        {
                            response.insert(QStringLiteral("paths"), savedFramePaths(session));
                            addFrameMetadata(response, frame);
                        }
                        else
                        {
                            response.insert(QStringLiteral("error"), session->saveMessage());
                        }
                        finish(std::move(response));
                        context->deleteLater();
                    });
            m_scopeonecore->saveRecordingSession(session);
            return true;
        }

        return false;
    }

    // Dispatches one local API request object
    QJsonObject ScopeOneLocalApiServer::processRequest(const QJsonObject& request)
    {
        const QString type = request.value(QStringLiteral("type")).toString().trimmed();
        if (type.isEmpty())
        {
            QJsonObject response = makeResponse(QStringLiteral("error"), false);
            response.insert(QStringLiteral("error"), QStringLiteral("Missing request type"));
            return response;
        }

        if (type == QStringLiteral("ping"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
            response.insert(QStringLiteral("coreVersion"), scopeone::core::ScopeOneCore::getVersion());
            return response;
        }

        if (type == QStringLiteral("version"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
            response.insert(QStringLiteral("coreVersion"), scopeone::core::ScopeOneCore::getVersion());
            return response;
        }

        if (type == QStringLiteral("capabilities"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("capabilities"), apiCapabilities());
            return response;
        }

        if (type == QStringLiteral("status"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
            response.insert(QStringLiteral("coreVersion"), scopeone::core::ScopeOneCore::getVersion());
            response.insert(QStringLiteral("configurationState"),
                            m_scopeonecore->configurationState());
            response.insert(QStringLiteral("configurationError"),
                            m_scopeonecore->configurationError());
            response.insert(QStringLiteral("configurationOperationRunning"),
                            m_scopeonecore->configurationOperationRunning());
            response.insert(QStringLiteral("configurationPath"),
                            m_scopeonecore->loadedConfigurationPath());
            response.insert(QStringLiteral("configurationComplete"),
                            m_scopeonecore->configurationState() == QStringLiteral("loaded"));
            response.insert(QStringLiteral("failedConfigurationDevices"),
                            QJsonArray::fromStringList(
                                m_scopeonecore->configurationFailedDevices()));
            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
            response.insert(QStringLiteral("loadedDevices"),
                            QJsonArray::fromStringList(m_scopeonecore->loadedDevices()));
            response.insert(QStringLiteral("runningPreviews"),
                            QJsonArray::fromStringList(m_scopeonecore->runningPreviewCameraIds()));
            response.insert(QStringLiteral("processingBitDepth"),
                            static_cast<int>(m_scopeonecore->processingBitDepth()));
            response.insert(QStringLiteral("processingRealTime"),
                            m_scopeonecore->isRealTimeProcessingEnabled());
            response.insert(QStringLiteral("processingModuleCount"),
                            static_cast<int>(m_scopeonecore->processingModules().size()));
            response.insert(QStringLiteral("layers"),
                            QJsonArray::fromStringList(m_sceneModel->layerIds()));
            response.insert(QStringLiteral("visibleLayers"),
                            QJsonArray::fromStringList(m_sceneModel->visibleLayerIds()));
            response.insert(QStringLiteral("layerLayout"),
                            layerLayoutModeName(m_previewWidget->layerLayoutMode()));
            response.insert(QStringLiteral("stageMosaic"),
                            stageMosaicStatusToJson(m_scopeonecore->stageMosaicStatus()));
            response.insert(QStringLiteral("recordingProgress"),
                            recordingProgressToJson(m_scopeonecore->recordingProgress()));
            response.insert(QStringLiteral("recordingWriter"),
                            recordingWriterStatusToJson(m_scopeonecore->recordingWriterStatus()));
            return response;
        }

        if (type == QStringLiteral("state_snapshot"))
        {
            QJsonObject application;
            application.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
            application.insert(QStringLiteral("coreVersion"), scopeone::core::ScopeOneCore::getVersion());

            const QString configPath = m_scopeonecore->loadedConfigurationPath();
            QJsonObject configuration;
            const QString configState = m_scopeonecore->configurationState();
            configuration.insert(QStringLiteral("state"), configState);
            configuration.insert(QStringLiteral("operationRunning"),
                                 m_scopeonecore->configurationOperationRunning());
            configuration.insert(QStringLiteral("loaded"),
                                 configState == QStringLiteral("loaded")
                                     || configState == QStringLiteral("partially_loaded"));
            configuration.insert(QStringLiteral("complete"),
                                 configState == QStringLiteral("loaded"));
            configuration.insert(QStringLiteral("path"), configPath);
            configuration.insert(QStringLiteral("sha256"), m_scopeonecore->loadedConfigurationSha256());
            configuration.insert(QStringLiteral("error"), m_scopeonecore->configurationError());
            configuration.insert(QStringLiteral("failedDevices"),
                                 QJsonArray::fromStringList(
                                     m_scopeonecore->configurationFailedDevices()));

            QJsonObject hardware;
            hardware.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
            hardware.insert(QStringLiteral("loadedDevices"),
                            QJsonArray::fromStringList(m_scopeonecore->loadedDevices()));
            hardware.insert(QStringLiteral("xyStageDevices"),
                            QJsonArray::fromStringList(m_scopeonecore->xyStageDevices()));
            hardware.insert(QStringLiteral("zStageDevices"),
                            QJsonArray::fromStringList(m_scopeonecore->zStageDevices()));
            hardware.insert(QStringLiteral("currentXYStageDevice"), m_scopeonecore->currentXYStageDevice());
            hardware.insert(QStringLiteral("currentFocusDevice"), m_scopeonecore->currentFocusDevice());

            QJsonObject preview;
            preview.insert(QStringLiteral("runningCameraIds"),
                           QJsonArray::fromStringList(m_scopeonecore->runningPreviewCameraIds()));
            preview.insert(QStringLiteral("layout"), layerLayoutModeName(m_previewWidget->layerLayoutMode()));
            preview.insert(QStringLiteral("visibleLayerKeys"),
                           QJsonArray::fromStringList(m_sceneModel->visibleLayerIds()));

            QJsonObject scene;
            scene.insert(QStringLiteral("layers"), layersToJson(m_sceneModel, m_previewWidget));
            scene.insert(QStringLiteral("markups"), markupsToJson(m_sceneModel));

            QJsonObject acquisition;
            acquisition.insert(QStringLiteral("recording"), m_scopeonecore->isRecording());
            const QString activeExperimentId = m_scopeonecore->activeExperimentId();
            const bool cancelRequested = m_scopeonecore->experimentCancelRequested();
            acquisition.insert(QStringLiteral("activeExperimentId"), activeExperimentId);
            acquisition.insert(QStringLiteral("cancelRequested"), cancelRequested);
            acquisition.insert(QStringLiteral("stageMosaic"),
                               stageMosaicStatusToJson(m_scopeonecore->stageMosaicStatus()));
            acquisition.insert(QStringLiteral("progress"),
                               recordingProgressToJson(m_scopeonecore->recordingProgress()));
            acquisition.insert(QStringLiteral("writer"),
                               recordingWriterStatusToJson(m_scopeonecore->recordingWriterStatus()));

            QJsonArray experiments;
            QStringList experimentIds = m_scopeonecore->experimentIds();
            experimentIds.sort();
            for (const QString& experimentId : experimentIds)
            {
                scopeone::core::ExperimentDocument document;
                if (!m_scopeonecore->experimentDocument(experimentId, document))
                {
                    continue;
                }
                QJsonObject experiment;
                experiment.insert(QStringLiteral("experimentId"), experimentId);
                experiment.insert(QStringLiteral("state"),
                                  scopeone::core::experimentRunStateName(document.runState));
                experiment.insert(QStringLiteral("active"), experimentId == activeExperimentId);
                experiment.insert(QStringLiteral("cancelRequested"),
                                  experimentId == activeExperimentId && cancelRequested);
                experiments.append(experiment);
            }

            QJsonArray sessions;
            QStringList sessionIds = m_scopeonecore->recordingSessionIds();
            sessionIds.sort();
            for (const QString& sessionId : sessionIds)
            {
                const auto session = m_scopeonecore->recordingSession(sessionId);
                if (!session)
                {
                    continue;
                }
                QJsonObject frameCounts;
                for (const QString& cameraId : session->recordedCameraIds())
                {
                    frameCounts.insert(cameraId, session->recordedFrameCount(cameraId));
                }
                QJsonObject sessionState;
                sessionState.insert(QStringLiteral("sessionId"), sessionId);
                sessionState.insert(QStringLiteral("cameraIds"),
                                    QJsonArray::fromStringList(session->recordedCameraIds()));
                sessionState.insert(QStringLiteral("frameCount"), session->recordedFrameCount());
                sessionState.insert(QStringLiteral("frameCounts"), frameCounts);
                sessionState.insert(QStringLiteral("saved"), session->isSaved());
                sessions.append(sessionState);
            }

            QJsonObject snapshot;
            snapshot.insert(QStringLiteral("timestampMs"),
                            static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
            snapshot.insert(QStringLiteral("application"), application);
            snapshot.insert(QStringLiteral("configuration"), configuration);
            snapshot.insert(QStringLiteral("hardware"), hardware);
            snapshot.insert(QStringLiteral("preview"), preview);
            snapshot.insert(QStringLiteral("processing"), processingStateToJson(m_scopeonecore));
            snapshot.insert(QStringLiteral("scene"), scene);
            snapshot.insert(QStringLiteral("acquisition"), acquisition);
            snapshot.insert(QStringLiteral("experiments"), experiments);
            snapshot.insert(QStringLiteral("sessions"), sessions);

            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("snapshot"), snapshot);
            return response;
        }

        if (type == QStringLiteral("frame_mapping_info"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            response.insert(QStringLiteral("headerBytes"), scopeone::core::kSharedFrameHeaderSize);
            response.insert(QStringLiteral("maxPayloadBytes"), scopeone::core::kSharedFrameMaxBytes);
            response.insert(QStringLiteral("pixelFormats"),
                            QJsonArray::fromStringList(QStringList{
                                QStringLiteral("Mono8"),
                                QStringLiteral("Mono16"),
                            }));
            return response;
        }

        if (type == QStringLiteral("camera_ids"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
            return response;
        }

        if (type == QStringLiteral("loaded_devices"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("devices"), QJsonArray::fromStringList(m_scopeonecore->loadedDevices()));
            return response;
        }

        if (type == QStringLiteral("start_preview"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
            const bool ok = m_scopeonecore->startPreview(camera);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to start preview"));
            }
            return response;
        }

        if (type == QStringLiteral("stop_preview"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
            const bool ok = m_scopeonecore->stopPreview(camera);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to stop preview"));
            }
            return response;
        }

        if (type == QStringLiteral("list_layers"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("layers"), layersToJson(m_sceneModel, m_previewWidget));
            return response;
        }

        if (type == QStringLiteral("get_layer_histogram"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            scopeone::core::ScopeOneCore::HistogramStats stats;
            QJsonObject response = makeResponse(
                type,
                !layerKey.isEmpty() && m_scopeonecore->getLayerHistogram(layerKey, stats));
            if (!response.value(QStringLiteral("ok")).toBool())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Layer has no current frame"));
                return response;
            }
            response.insert(QStringLiteral("layerKey"), layerKey);
            response.insert(QStringLiteral("histogram"), histogramToJson(stats));
            return response;
        }

        if (type == QStringLiteral("get_pixel_value"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            int x = 0;
            int y = 0;
            int value = 0;
            QJsonObject response = makeResponse(type, false);
            if (layerKey.isEmpty()
                || !intField(request, QStringLiteral("x"), x)
                || !intField(request, QStringLiteral("y"), y)
                || !m_scopeonecore->graphPixelValue(layerKey, QPoint(x, y), value))
            {
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Layer has no current frame or position is outside the image"));
                return response;
            }
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            response.insert(QStringLiteral("x"), x);
            response.insert(QStringLiteral("y"), y);
            response.insert(QStringLiteral("value"), value);
            return response;
        }

        if (type == QStringLiteral("auto_layer_levels")
            || type == QStringLiteral("full_layer_levels"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            scopeone::core::DocumentLayer layer;
            QJsonObject response = makeResponse(type, false);
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }
            const bool ok = type == QStringLiteral("auto_layer_levels")
                                ? m_scopeonecore->autoLayerLevels(layerKey)
                                : m_scopeonecore->fullLayerLevels(layerKey);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Layer has no current frame"));
                return response;
            }
            m_sceneModel->findLayer(layerKey, layer);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            insertLayerDisplayFields(
                response, layer, m_scopeonecore->layerAutoStretchEnabled(layerKey));
            return response;
        }

        if (type == QStringLiteral("set_layer_auto_stretch"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            const QJsonValue enabledValue = request.value(QStringLiteral("enabled"));
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }
            if (!enabledValue.isBool())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing enabled value"));
                return response;
            }
            m_scopeonecore->setLayerAutoStretchEnabled(layerKey, enabledValue.toBool());
            m_sceneModel->findLayer(layerKey, layer);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            insertLayerDisplayFields(
                response, layer, m_scopeonecore->layerAutoStretchEnabled(layerKey));
            return response;
        }

        if (type == QStringLiteral("get_line_profile"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            int x1 = 0;
            int y1 = 0;
            int x2 = 0;
            int y2 = 0;
            QJsonObject response = makeResponse(type, false);
            if (layerKey.isEmpty()
                || !intField(request, QStringLiteral("x1"), x1)
                || !intField(request, QStringLiteral("y1"), y1)
                || !intField(request, QStringLiteral("x2"), x2)
                || !intField(request, QStringLiteral("y2"), y2)
                || QPoint(x1, y1) == QPoint(x2, y2))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or invalid line geometry"));
                return response;
            }

            QVector<int> values;
            if (!m_scopeonecore->getLineProfile(layerKey,
                                                QPoint(x1, y1),
                                                QPoint(x2, y2),
                                                values))
            {
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Layer has no current frame or line is outside the image"));
                return response;
            }
            QJsonArray samples;
            for (const int value : values)
            {
                samples.append(value);
            }
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            response.insert(QStringLiteral("values"), samples);
            return response;
        }

        if (type == QStringLiteral("layer_options"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("layouts"),
                            QJsonArray::fromStringList(QStringList{
                                QStringLiteral("side_by_side"),
                                QStringLiteral("overlay"),
                            }));
            response.insert(QStringLiteral("colormaps"),
                            QJsonArray::fromStringList(m_previewWidget->supportedLayerColormaps()));
            response.insert(QStringLiteral("blendingModes"),
                            QJsonArray::fromStringList(m_previewWidget->supportedLayerBlendingModes()));
            return response;
        }

        if (type == QStringLiteral("set_layer_layout"))
        {
            PreviewWidget::LayerLayoutMode mode = PreviewWidget::LayerLayoutMode::SideBySide;
            QJsonObject response = makeResponse(type, false);
            if (!layerLayoutModeFromName(request.value(QStringLiteral("layout")).toString(), mode))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown layer layout"));
                return response;
            }

            m_previewWidget->setLayerLayoutMode(mode);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layout"), layerLayoutModeName(m_previewWidget->layerLayoutMode()));
            return response;
        }

        if (type == QStringLiteral("set_visible_layers"))
        {
            QJsonObject response = makeResponse(type, false);
            if (!request.value(QStringLiteral("layerKeys")).isArray())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing layerKeys"));
                return response;
            }

            const QStringList layerKeys = stringArrayFromJson(request.value(QStringLiteral("layerKeys")).toArray());
            for (const QString& layerKey : layerKeys)
            {
                scopeone::core::DocumentLayer layer;
                if (!m_sceneModel->findLayer(layerKey, layer))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Unknown layerKey: %1").arg(layerKey));
                    return response;
                }
            }

            m_sceneModel->setVisibleLayers(layerKeys);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("visibleLayers"),
                            QJsonArray::fromStringList(m_sceneModel->visibleLayerIds()));
            return response;
        }

        if (type == QStringLiteral("get_source_display_transform"))
        {
            const QString sourceId = request.value(QStringLiteral("sourceId")).toString().trimmed();
            QJsonObject response = makeResponse(type, m_sceneModel->hasSource(sourceId));
            if (!response.value(QStringLiteral("ok")).toBool())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown sourceId"));
                return response;
            }
            const ImageSceneModel::SourceDisplayTransform transform =
                m_sceneModel->sourceDisplayTransform(sourceId);
            response.insert(QStringLiteral("sourceId"), sourceId);
            response.insert(QStringLiteral("offsetX"), transform.offsetX);
            response.insert(QStringLiteral("offsetY"), transform.offsetY);
            response.insert(QStringLiteral("zoomPercent"), transform.zoomPercent);
            response.insert(QStringLiteral("flipX"), transform.flipX);
            response.insert(QStringLiteral("flipY"), transform.flipY);
            return response;
        }

        if (type == QStringLiteral("set_source_display_transform"))
        {
            const QString sourceId = request.value(QStringLiteral("sourceId")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (!m_sceneModel->hasSource(sourceId))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown sourceId"));
                return response;
            }
            ImageSceneModel::SourceDisplayTransform transform =
                m_sceneModel->sourceDisplayTransform(sourceId);
            if ((request.contains(QStringLiteral("offsetX"))
                 && !intField(request, QStringLiteral("offsetX"), transform.offsetX))
                || (request.contains(QStringLiteral("offsetY"))
                    && !intField(request, QStringLiteral("offsetY"), transform.offsetY))
                || (request.contains(QStringLiteral("zoomPercent"))
                    && !intField(request, QStringLiteral("zoomPercent"), transform.zoomPercent)))
            {
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Display transform values must be integers"));
                return response;
            }
            const QStringList booleanFields{QStringLiteral("flipX"), QStringLiteral("flipY")};
            for (const QString& field : booleanFields)
            {
                if (request.contains(field) && !request.value(field).isBool())
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("%1 must be a boolean").arg(field));
                    return response;
                }
            }

            transform.flipX = request.value(QStringLiteral("flipX")).toBool(transform.flipX);
            transform.flipY = request.value(QStringLiteral("flipY")).toBool(transform.flipY);
            m_sceneModel->setSourceDisplayTransform(sourceId, transform);
            transform = m_sceneModel->sourceDisplayTransform(sourceId);

            response = makeResponse(type, true);
            response.insert(QStringLiteral("sourceId"), sourceId);
            response.insert(QStringLiteral("offsetX"), transform.offsetX);
            response.insert(QStringLiteral("offsetY"), transform.offsetY);
            response.insert(QStringLiteral("zoomPercent"), transform.zoomPercent);
            response.insert(QStringLiteral("flipX"), transform.flipX);
            response.insert(QStringLiteral("flipY"), transform.flipY);
            return response;
        }

        if (type == QStringLiteral("reset_source_display_transform"))
        {
            const QString sourceId = request.value(QStringLiteral("sourceId")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (!m_sceneModel->resetSourceDisplayTransform(sourceId))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown sourceId"));
                return response;
            }
            const ImageSceneModel::SourceDisplayTransform transform =
                m_sceneModel->sourceDisplayTransform(sourceId);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("sourceId"), sourceId);
            response.insert(QStringLiteral("offsetX"), transform.offsetX);
            response.insert(QStringLiteral("offsetY"), transform.offsetY);
            response.insert(QStringLiteral("zoomPercent"), transform.zoomPercent);
            response.insert(QStringLiteral("flipX"), transform.flipX);
            response.insert(QStringLiteral("flipY"), transform.flipY);
            return response;
        }

        if (type == QStringLiteral("set_layer_display"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            const bool hasColormap = request.contains(QStringLiteral("colormap"));
            const QString colormap = request.value(QStringLiteral("colormap")).toString().trimmed();
            const bool hasBlending = request.contains(QStringLiteral("blending"));
            const QString blending = request.value(QStringLiteral("blending")).toString().trimmed();
            const bool hasLevels = request.contains(QStringLiteral("minLevel"))
                                   || request.contains(QStringLiteral("maxLevel"))
                                   || request.contains(QStringLiteral("maxPossible"));
            int minLevel = 0;
            int maxLevel = 0;
            int maxPossible = 0;
            int opacityPercent = 100;
            const QJsonValue visibleValue = request.value(QStringLiteral("visible"));
            const QJsonValue gammaValue = request.value(QStringLiteral("gamma"));

            if ((request.contains(QStringLiteral("visible")) && !visibleValue.isBool())
                || (request.contains(QStringLiteral("opacityPercent"))
                    && !intField(request, QStringLiteral("opacityPercent"), opacityPercent))
                || (request.contains(QStringLiteral("gamma"))
                    && (!gammaValue.isDouble() || !std::isfinite(gammaValue.toDouble()))))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid layer display value"));
                return response;
            }

            if (hasColormap)
            {
                if (!m_previewWidget->supportedLayerColormaps().contains(colormap, Qt::CaseInsensitive))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Unsupported colormap"));
                    return response;
                }
            }
            if (hasBlending)
            {
                if (!m_previewWidget->supportedLayerBlendingModes().contains(blending, Qt::CaseInsensitive))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Unsupported blending mode"));
                    return response;
                }
            }
            if (hasLevels)
            {
                if (!intField(request, QStringLiteral("minLevel"), minLevel)
                    || !intField(request, QStringLiteral("maxLevel"), maxLevel)
                    || !intField(request, QStringLiteral("maxPossible"), maxPossible))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Display levels require minLevel, maxLevel, and maxPossible"));
                    return response;
                }
            }

            if (request.contains(QStringLiteral("visible")))
            {
                m_sceneModel->setLayerVisible(layerKey, visibleValue.toBool());
            }
            if (request.contains(QStringLiteral("opacityPercent")))
            {
                m_sceneModel->setLayerOpacityPercent(layerKey, opacityPercent);
            }
            if (request.contains(QStringLiteral("gamma")))
            {
                m_sceneModel->setLayerGamma(layerKey, gammaValue.toDouble());
            }
            if (hasColormap)
            {
                m_sceneModel->setLayerColormap(layerKey, colormap);
            }
            if (hasBlending)
            {
                m_sceneModel->setLayerBlending(layerKey, blending);
            }
            if (hasLevels)
            {
                m_scopeonecore->setLayerAutoStretchEnabled(layerKey, false);
                m_sceneModel->setLayerDisplayLevels(layerKey, minLevel, maxLevel, maxPossible);
            }

            m_sceneModel->findLayer(layerKey, layer);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            insertLayerDisplayFields(
                response, layer, m_scopeonecore->layerAutoStretchEnabled(layerKey));
            return response;
        }

        if (type == QStringLiteral("move_layer"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            const int offset = request.value(QStringLiteral("offset")).toInt(0);
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            const bool moved = m_sceneModel->moveLayer(layerKey, offset);
            response = makeResponse(type, true);
            response.insert(QStringLiteral("moved"), moved);
            response.insert(QStringLiteral("layers"),
                            QJsonArray::fromStringList(m_sceneModel->layerIds()));
            return response;
        }

        if (type == QStringLiteral("config_groups"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("groups"),
                            QJsonArray::fromStringList(m_scopeonecore->availableConfigGroups()));
            return response;
        }

        if (type == QStringLiteral("configs"))
        {
            const QString group = request.value(QStringLiteral("group")).toString().trimmed();
            QJsonObject response = makeResponse(type, !group.isEmpty());
            if (group.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing group"));
                return response;
            }
            response.insert(QStringLiteral("configs"),
                            QJsonArray::fromStringList(m_scopeonecore->availableConfigs(group)));
            return response;
        }

        if (type == QStringLiteral("current_config"))
        {
            const QString group = request.value(QStringLiteral("group")).toString().trimmed();
            QJsonObject response = makeResponse(type, !group.isEmpty());
            if (group.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing group"));
                return response;
            }
            response.insert(QStringLiteral("config"), m_scopeonecore->currentConfig(group));
            return response;
        }

        if (type == QStringLiteral("set_config"))
        {
            const QString group = request.value(QStringLiteral("group")).toString().trimmed();
            const QString config = request.value(QStringLiteral("config")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (group.isEmpty() || config.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing group or config"));
                return response;
            }
            QString errorMessage;
            if (!m_scopeonecore->setConfig(group, config, &errorMessage))
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            response = makeResponse(type, true);
            response.insert(QStringLiteral("config"), m_scopeonecore->currentConfig(group));
            return response;
        }

        if (type == QStringLiteral("remove_static_layer"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            const QString sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey).trimmed();
            QJsonObject response = makeResponse(type, false);
            if (!scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey)
                || sourceId.isEmpty()
                || !m_previewWidget->availableLayerKeys().contains(layerKey))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown static layerKey"));
                return response;
            }

            m_scopeonecore->removeStaticFrame(sourceId);
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("clear_static_layers"))
        {
            m_scopeonecore->clearStaticFrames();
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("create_line_markup"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            int x1 = 0;
            int y1 = 0;
            int x2 = 0;
            int y2 = 0;
            if (!intField(request, QStringLiteral("x1"), x1)
                || !intField(request, QStringLiteral("y1"), y1)
                || !intField(request, QStringLiteral("x2"), x2)
                || !intField(request, QStringLiteral("y2"), y2))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing markup geometry"));
                return response;
            }

            const ImageSceneModel::MarkupRole role = markupRoleFromJson(request.value(QStringLiteral("role")));
            if (role == ImageSceneModel::MarkupRole::Roi)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup role for line"));
                return response;
            }

            const QPoint start(x1, y1);
            const QPoint end(x2, y2);
            if (start == end)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup geometry"));
                return response;
            }

            const QString id = m_sceneModel->createLine(
                layerKey,
                start,
                end,
                request.value(QStringLiteral("label")).toString(),
                role);
            if (id.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup geometry or layer"));
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("markupId"), id);
            return response;
        }

        if (type == QStringLiteral("create_rect_markup"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (layerKey.isEmpty() || !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (!intField(request, QStringLiteral("x"), x)
                || !intField(request, QStringLiteral("y"), y)
                || !intField(request, QStringLiteral("width"), width)
                || !intField(request, QStringLiteral("height"), height))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing markup geometry"));
                return response;
            }
            if (width <= 0 || height <= 0)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup geometry"));
                return response;
            }

            const ImageSceneModel::MarkupRole role = markupRoleFromJson(request.value(QStringLiteral("role")));
            if (role == ImageSceneModel::MarkupRole::CrossSection)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup role for rect"));
                return response;
            }

            const QString id = m_sceneModel->createRect(
                layerKey,
                QRect(x, y, width, height),
                request.value(QStringLiteral("label")).toString(),
                role);
            if (id.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup geometry or layer"));
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("markupId"), id);
            return response;
        }

        if (type == QStringLiteral("list_markups"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (!layerKey.isEmpty() && !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            QJsonArray markups;
            for (const ImageSceneModel::Markup& markup : m_sceneModel->markups(layerKey))
            {
                markups.append(markupToJson(markup));
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("markups"), markups);
            return response;
        }

        if (type == QStringLiteral("update_markup"))
        {
            const QString markupId = request.value(QStringLiteral("markupId")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            ImageSceneModel::Markup markup;
            if (markupId.isEmpty() || !m_sceneModel->findMarkup(markupId, markup))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown markup"));
                return response;
            }

            if (request.contains(QStringLiteral("label"))
                && !m_sceneModel->setLabel(markupId, request.value(QStringLiteral("label")).toString()))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to update markup label"));
                return response;
            }
            if (request.contains(QStringLiteral("visible"))
                && !m_sceneModel->setVisible(markupId, request.value(QStringLiteral("visible")).toBool(true)))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to update markup visibility"));
                return response;
            }
            if (request.contains(QStringLiteral("selected")))
            {
                const bool selected = request.value(QStringLiteral("selected")).toBool(false);
                if (selected)
                {
                    if (!m_sceneModel->selectOnly(markupId))
                    {
                        response.insert(QStringLiteral("error"), QStringLiteral("Failed to select markup"));
                        return response;
                    }
                }
                else if (!m_sceneModel->setSelected(markupId, false))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Failed to update markup selection"));
                    return response;
                }
            }

            const bool hasLineGeometry = request.contains(QStringLiteral("x1"))
                || request.contains(QStringLiteral("y1"))
                || request.contains(QStringLiteral("x2"))
                || request.contains(QStringLiteral("y2"));
            const bool hasRectGeometry = request.contains(QStringLiteral("x"))
                || request.contains(QStringLiteral("y"))
                || request.contains(QStringLiteral("width"))
                || request.contains(QStringLiteral("height"));
            if ((markup.type == ImageSceneModel::MarkupType::Line && hasRectGeometry)
                || (markup.type == ImageSceneModel::MarkupType::Rect && hasLineGeometry))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid markup geometry for type"));
                return response;
            }

            if (markup.type == ImageSceneModel::MarkupType::Line && hasLineGeometry)
            {
                int x1 = markup.start.x();
                int y1 = markup.start.y();
                int x2 = markup.end.x();
                int y2 = markup.end.y();
                if ((request.contains(QStringLiteral("x1")) && !intField(request, QStringLiteral("x1"), x1))
                    || (request.contains(QStringLiteral("y1")) && !intField(request, QStringLiteral("y1"), y1))
                    || (request.contains(QStringLiteral("x2")) && !intField(request, QStringLiteral("x2"), x2))
                    || (request.contains(QStringLiteral("y2")) && !intField(request, QStringLiteral("y2"), y2))
                    || !m_sceneModel->updateLine(markupId, QPoint(x1, y1), QPoint(x2, y2)))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Invalid line markup geometry"));
                    return response;
                }
            }

            if (markup.type == ImageSceneModel::MarkupType::Rect && hasRectGeometry)
            {
                int x = markup.rect.x();
                int y = markup.rect.y();
                int width = markup.rect.width();
                int height = markup.rect.height();
                if ((request.contains(QStringLiteral("x")) && !intField(request, QStringLiteral("x"), x))
                    || (request.contains(QStringLiteral("y")) && !intField(request, QStringLiteral("y"), y))
                    || (request.contains(QStringLiteral("width")) && !intField(request, QStringLiteral("width"), width))
                    || (request.contains(QStringLiteral("height")) && !intField(request, QStringLiteral("height"), height))
                    || !m_sceneModel->updateRect(markupId, QRect(x, y, width, height)))
                {
                    response.insert(QStringLiteral("error"), QStringLiteral("Invalid rect markup geometry"));
                    return response;
                }
            }

            if (!m_sceneModel->findMarkup(markupId, markup))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown markup"));
                return response;
            }
            response = makeResponse(type, true);
            response.insert(QStringLiteral("markup"), markupToJson(markup));
            return response;
        }

        if (type == QStringLiteral("remove_markup"))
        {
            const QString markupId = request.value(QStringLiteral("markupId")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (markupId.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing markupId"));
                return response;
            }
            if (!m_sceneModel->remove(markupId))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown markup"));
                return response;
            }
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("clear_markups"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            scopeone::core::DocumentLayer layer;
            if (!layerKey.isEmpty() && !m_sceneModel->findLayer(layerKey, layer))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            m_sceneModel->clear(layerKey);
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("device_properties"))
        {
            const QString device = request.value(QStringLiteral("device")).toString().trimmed();
            const bool fromCache = request.value(QStringLiteral("fromCache")).toBool(true);
            QJsonObject response = makeResponse(type, !device.isEmpty());
            if (device.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing device"));
                return response;
            }

            QJsonArray properties;
            for (const auto& info : m_scopeonecore->deviceProperties(device, fromCache))
            {
                properties.append(propertyInfoToJson(info));
            }
            response.insert(QStringLiteral("properties"), properties);
            return response;
        }

        if (type == QStringLiteral("device_property_names"))
        {
            const QString device = request.value(QStringLiteral("device")).toString().trimmed();
            QJsonObject response = makeResponse(type, !device.isEmpty());
            if (device.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing device"));
                return response;
            }
            response.insert(QStringLiteral("names"),
                            QJsonArray::fromStringList(m_scopeonecore->devicePropertyNames(device)));
            return response;
        }

        if (type == QStringLiteral("get_property"))
        {
            const QString device = request.value(QStringLiteral("device")).toString().trimmed();
            const QString property = request.value(QStringLiteral("property")).toString().trimmed();
            const bool fromCache = request.value(QStringLiteral("fromCache")).toBool(true);
            QJsonObject response = makeResponse(type, !device.isEmpty() && !property.isEmpty());
            if (device.isEmpty() || property.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing device or property"));
                return response;
            }
            response.insert(QStringLiteral("value"), m_scopeonecore->getPropertyValue(device, property, fromCache));
            return response;
        }

        if (type == QStringLiteral("set_property"))
        {
            QString errorMessage;
            const QString device = request.value(QStringLiteral("device")).toString().trimmed();
            const QString property = request.value(QStringLiteral("property")).toString().trimmed();
            const QString value = request.value(QStringLiteral("value")).toString();
            const bool ok = !device.isEmpty()
                && !property.isEmpty()
                && m_scopeonecore->setPropertyValue(device, property, value, &errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to set property")
                                    : errorMessage);
            }
            return response;
        }

        if (type == QStringLiteral("read_exposure"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
            double exposureMs = 0.0;
            QJsonObject response = makeResponse(type, m_scopeonecore->readExposure(camera, exposureMs));
            if (response.value(QStringLiteral("ok")).toBool())
            {
                response.insert(QStringLiteral("exposureMs"), exposureMs);
            }
            else
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to read exposure"));
            }
            return response;
        }

        if (type == QStringLiteral("set_exposure"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
            const QJsonValue exposureValue = request.value(QStringLiteral("exposureMs"));
            QJsonObject response = makeResponse(type, false);
            if (!exposureValue.isDouble())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing exposureMs"));
                return response;
            }
            if (!m_scopeonecore->setExposure(camera, exposureValue.toDouble()))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to set exposure"));
                return response;
            }

            double exposureMs = 0.0;
            response = makeResponse(type, true);
            if (m_scopeonecore->readExposure(camera, exposureMs))
            {
                response.insert(QStringLiteral("exposureMs"), exposureMs);
            }
            return response;
        }

        if (type == QStringLiteral("get_roi"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (camera.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing camera"));
                return response;
            }

            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (!m_scopeonecore->getROI(camera, x, y, width, height))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to read ROI"));
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("x"), x);
            response.insert(QStringLiteral("y"), y);
            response.insert(QStringLiteral("width"), width);
            response.insert(QStringLiteral("height"), height);
            return response;
        }

        if (type == QStringLiteral("set_roi"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (camera.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing camera"));
                return response;
            }

            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (!intField(request, QStringLiteral("x"), x)
                || !intField(request, QStringLiteral("y"), y)
                || !intField(request, QStringLiteral("width"), width)
                || !intField(request, QStringLiteral("height"), height)
                || width <= 0
                || height <= 0)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid ROI"));
                return response;
            }
            if (!m_scopeonecore->setROI(camera, x, y, width, height))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to set ROI"));
                return response;
            }

            response = makeResponse(type, true);
            if (m_scopeonecore->getROI(camera, x, y, width, height))
            {
                response.insert(QStringLiteral("readBack"), true);
            }
            else
            {
                response.insert(QStringLiteral("readBack"), false);
            }
            response.insert(QStringLiteral("x"), x);
            response.insert(QStringLiteral("y"), y);
            response.insert(QStringLiteral("width"), width);
            response.insert(QStringLiteral("height"), height);
            return response;
        }

        if (type == QStringLiteral("set_half_roi"))
        {
            const QString camera = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (camera.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing camera"));
                return response;
            }
            if (!m_scopeonecore->setHalfROI(camera))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to set Half ROI"));
                return response;
            }

            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            response = makeResponse(type, true);
            response.insert(QStringLiteral("readBack"),
                            m_scopeonecore->getROI(camera, x, y, width, height));
            response.insert(QStringLiteral("x"), x);
            response.insert(QStringLiteral("y"), y);
            response.insert(QStringLiteral("width"), width);
            response.insert(QStringLiteral("height"), height);
            return response;
        }

        if (type == QStringLiteral("clear_roi"))
        {
            const QString camera = request.value(QStringLiteral("camera"))
                                       .toString(QStringLiteral("All"))
                                       .trimmed();
            QJsonObject response = makeResponse(type, false);
            if (camera.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing camera"));
                return response;
            }
            if (!m_scopeonecore->clearROI(camera))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to clear ROI"));
                return response;
            }
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("xy_stage_devices"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("devices"), QJsonArray::fromStringList(m_scopeonecore->xyStageDevices()));
            return response;
        }

        if (type == QStringLiteral("z_stage_devices"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("devices"), QJsonArray::fromStringList(m_scopeonecore->zStageDevices()));
            return response;
        }

        if (type == QStringLiteral("current_xy_stage_device"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("device"), m_scopeonecore->currentXYStageDevice());
            return response;
        }

        if (type == QStringLiteral("current_focus_device"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("device"), m_scopeonecore->currentFocusDevice());
            return response;
        }

        if (type == QStringLiteral("read_xy_position"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentXYStageDevice());
            double x = 0.0;
            double y = 0.0;
            const bool ok = m_scopeonecore->readXYPosition(device, x, y);
            QJsonObject response = makeResponse(type, ok);
            if (ok)
            {
                response.insert(QStringLiteral("x"), x);
                response.insert(QStringLiteral("y"), y);
            }
            else
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to read XY position"));
            }
            return response;
        }

        if (type == QStringLiteral("read_z_position"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentFocusDevice());
            double z = 0.0;
            const bool ok = m_scopeonecore->readZPosition(device, z);
            QJsonObject response = makeResponse(type, ok);
            if (ok)
            {
                response.insert(QStringLiteral("z"), z);
            }
            else
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to read Z position"));
            }
            return response;
        }

        if (type == QStringLiteral("start_stage_mosaic"))
        {
            scopeone::core::ScopeOneCore::StageMosaicPlan plan;
            plan.cameraId = request.value(QStringLiteral("cameraId")).toString().trimmed();
            plan.xyStageId = request.value(QStringLiteral("xyStageId")).toString().trimmed();

            auto readOptionalInt = [&request](const QString& key, int& value)
            {
                return !request.contains(key) || intField(request, key, value);
            };
            auto readOptionalDouble = [&request](const QString& key, double& value)
            {
                if (!request.contains(key))
                {
                    return true;
                }
                const QJsonValue field = request.value(key);
                if (!field.isDouble() || !std::isfinite(field.toDouble()))
                {
                    return false;
                }
                value = field.toDouble();
                return true;
            };

            const QJsonValue returnToStartValue = request.value(QStringLiteral("returnToStart"));
            const QJsonValue gallerySaveDirValue = request.value(QStringLiteral("gallerySaveDir"));
            if (plan.cameraId.isEmpty()
                || plan.xyStageId.isEmpty()
                || !readOptionalInt(QStringLiteral("rows"), plan.rows)
                || !readOptionalInt(QStringLiteral("columns"), plan.columns)
                || !readOptionalDouble(QStringLiteral("stepXUm"), plan.stepXUm)
                || !readOptionalDouble(QStringLiteral("stepYUm"), plan.stepYUm)
                || !readOptionalInt(QStringLiteral("settleMs"), plan.settleMs)
                || (!returnToStartValue.isUndefined() && !returnToStartValue.isBool())
                || (!gallerySaveDirValue.isUndefined() && !gallerySaveDirValue.isString()))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("Invalid stage mosaic request"));
                return response;
            }
            plan.returnToStart = returnToStartValue.toBool(true);
            plan.gallerySaveDir = gallerySaveDirValue.toString().trimmed();

            QString errorMessage;
            if (!m_scopeonecore->startStageMosaic(plan, &errorMessage))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("status"),
                            stageMosaicStatusToJson(m_scopeonecore->stageMosaicStatus()));
            return response;
        }

        if (type == QStringLiteral("stage_mosaic_status"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("status"),
                            stageMosaicStatusToJson(m_scopeonecore->stageMosaicStatus()));
            return response;
        }

        if (type == QStringLiteral("cancel_stage_mosaic"))
        {
            if (!m_scopeonecore->isStageMosaicRunning())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("No stage mosaic is running"));
                return response;
            }
            m_scopeonecore->cancelStageMosaic();
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("status"),
                            stageMosaicStatusToJson(m_scopeonecore->stageMosaicStatus()));
            return response;
        }

        if (type == QStringLiteral("processing_modules"))
        {
            const QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo> modules =
                m_scopeonecore->processingModules();
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("bitDepth"),
                            static_cast<int>(m_scopeonecore->processingBitDepth()));
            response.insert(QStringLiteral("realTime"),
                            m_scopeonecore->isRealTimeProcessingEnabled());
            response.insert(QStringLiteral("realTimeSource"),
                            m_scopeonecore->realTimeProcessingSource());
            response.insert(QStringLiteral("modules"), processingModulesToJson(modules));
            QJsonArray availableModules;
            for (const auto& descriptor : m_scopeonecore->availableProcessingModules())
            {
                availableModules.append(processingModuleDescriptorToJson(descriptor));
            }
            response.insert(QStringLiteral("availableModules"), availableModules);
            return response;
        }

        if (type == QStringLiteral("set_processing_bit_depth"))
        {
            const int bitDepth = request.value(QStringLiteral("bitDepth")).toInt(0);
            const bool ok = bitDepth == 8 || bitDepth == 16;
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("bitDepth must be 8 or 16"));
                return response;
            }
            const auto depth = bitDepth == 16
                                   ? scopeone::core::ScopeOneCore::ProcessingBitDepth::Bit16
                                   : scopeone::core::ScopeOneCore::ProcessingBitDepth::Bit8;
            if (!m_scopeonecore->setProcessingBitDepth(depth))
            {
                response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                m_scopeonecore->isRealTimeProcessingEnabled()
                                    ? QStringLiteral("Stop real-time processing before editing the pipeline")
                                    : QStringLiteral("Failed to set processing bit depth"));
                return response;
            }
            response.insert(QStringLiteral("bitDepth"), bitDepth);
            return response;
        }

        if (type == QStringLiteral("set_realtime_processing"))
        {
            const bool enabled = request.value(QStringLiteral("enabled")).toBool(false);
            if (enabled && request.contains(QStringLiteral("cameraId"))
                && !m_scopeonecore->setRealTimeProcessingSource(
                    request.value(QStringLiteral("cameraId")).toString()))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Unknown processing source or processing is already running"));
                return response;
            }
            if (!m_scopeonecore->setRealTimeProcessingEnabled(enabled))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                enabled
                                    ? QStringLiteral("Add a processing module before starting real-time processing")
                                    : QStringLiteral("Failed to stop real-time processing"));
                return response;
            }
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("realTime"), m_scopeonecore->isRealTimeProcessingEnabled());
            response.insert(QStringLiteral("realTimeSource"),
                            m_scopeonecore->realTimeProcessingSource());
            return response;
        }

        if (type == QStringLiteral("add_processing_module"))
        {
            const QString moduleId = processingModuleIdFromJson(request.value(QStringLiteral("kind")));
            QJsonObject response = makeResponse(type, false);
            if (moduleId.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown processing module kind"));
                return response;
            }
            if (!m_scopeonecore->addProcessingModule(moduleId))
            {
                response.insert(QStringLiteral("error"),
                                m_scopeonecore->isRealTimeProcessingEnabled()
                                    ? QStringLiteral("Stop real-time processing before editing the pipeline")
                                    : QStringLiteral("Failed to add processing module"));
                return response;
            }

            const QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo> modules =
                m_scopeonecore->processingModules();
            const int index = modules.size() - 1;
            if (request.value(QStringLiteral("parameters")).isObject()
                && !m_scopeonecore->setProcessingModuleParameters(
                    index,
                    request.value(QStringLiteral("parameters")).toObject().toVariantMap()))
            {
                m_scopeonecore->removeProcessingModule(index);
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to set processing module parameters"));
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("index"), index);
            response.insert(QStringLiteral("modules"), processingModulesToJson(m_scopeonecore->processingModules()));
            return response;
        }

        if (type == QStringLiteral("remove_processing_module"))
        {
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            QJsonObject response = makeResponse(type, false);
            if (!m_scopeonecore->removeProcessingModule(index))
            {
                response.insert(QStringLiteral("error"),
                                m_scopeonecore->isRealTimeProcessingEnabled()
                                    ? QStringLiteral("Stop real-time processing before editing the pipeline")
                                    : QStringLiteral("Failed to remove processing module"));
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("modules"), processingModulesToJson(m_scopeonecore->processingModules()));
            return response;
        }

        if (type == QStringLiteral("set_processing_module_parameters"))
        {
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            QJsonObject response = makeResponse(type, false);
            if (!request.value(QStringLiteral("parameters")).isObject())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing parameters"));
                return response;
            }
            if (!m_scopeonecore->setProcessingModuleParameters(
                index,
                request.value(QStringLiteral("parameters")).toObject().toVariantMap()))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to set processing module parameters"));
                if (m_scopeonecore->isRealTimeProcessingEnabled())
                {
                    response.insert(QStringLiteral("error"),
                                    QStringLiteral("Stop real-time processing before editing the pipeline"));
                }
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("modules"), processingModulesToJson(m_scopeonecore->processingModules()));
            return response;
        }

        if (type == QStringLiteral("reset_processing_module_state"))
        {
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            QJsonObject response = makeResponse(type, m_scopeonecore->resetProcessingModuleState(index));
            if (!response.value(QStringLiteral("ok")).toBool())
            {
                response.insert(QStringLiteral("error"),
                                m_scopeonecore->isRealTimeProcessingEnabled()
                                    ? QStringLiteral("Stop real-time processing before editing the pipeline")
                                    : QStringLiteral("Failed to reset processing module state"));
            }
            return response;
        }

        if (type == QStringLiteral("experiment_document"))
        {
            const scopeone::core::ExperimentDocument document = createExperimentDocument();
            QString errorMessage;
            const bool ok = scopeone::core::validateExperimentDocument(document, &errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            response.insert(QStringLiteral("document"),
                            scopeone::core::experimentDocumentToJson(document));
            return response;
        }

        if (type == QStringLiteral("validate_experiment"))
        {
            scopeone::core::ExperimentDocument document;
            QString errorMessage;
            const bool ok = experimentDocumentFromRequest(request, document, errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            response.insert(QStringLiteral("valid"), true);
            response.insert(QStringLiteral("document"),
                            scopeone::core::experimentDocumentToJson(document));
            return response;
        }

        if (type == QStringLiteral("save_experiment"))
        {
            scopeone::core::ExperimentDocument document;
            QString errorMessage;
            const QString filePath = request.value(QStringLiteral("filePath")).toString().trimmed();
            const bool ok = experimentDocumentFromRequest(request, document, errorMessage)
                && scopeone::core::saveExperimentDocument(filePath, document, &errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            response.insert(QStringLiteral("filePath"), filePath);
            return response;
        }

        if (type == QStringLiteral("load_experiment"))
        {
            if (!m_scopeonecore->activeExperimentId().isEmpty())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"),
                                QStringLiteral("Cannot replace the experiment document while an experiment is running"));
                return response;
            }
            scopeone::core::ExperimentDocument document;
            QString errorMessage;
            const QString filePath = request.value(QStringLiteral("filePath")).toString().trimmed();
            const bool ok = scopeone::core::loadExperimentDocument(filePath, document, &errorMessage)
                && m_sceneModel->setDocument(document, &errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            response.insert(QStringLiteral("filePath"), filePath);
            response.insert(QStringLiteral("document"),
                            scopeone::core::experimentDocumentToJson(document));
            return response;
        }

        if (type == QStringLiteral("start_experiment"))
        {
            scopeone::core::ExperimentDocument document;
            QString errorMessage;
            QJsonObject response = makeResponse(type, false);
            if (!experimentDocumentFromRequest(request, document, errorMessage))
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            const QString experimentId = document.plan.experimentId;
            if (!m_scopeonecore->startExperiment(document, &errorMessage))
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            return experimentStatusResponse(type, experimentId);
        }

        if (type == QStringLiteral("experiment_status"))
        {
            return experimentStatusResponse(
                type,
                request.value(QStringLiteral("experimentId")).toString().trimmed());
        }

        if (type == QStringLiteral("cancel_experiment"))
        {
            const QString experimentId =
                request.value(QStringLiteral("experimentId")).toString().trimmed();
            if (experimentId.isEmpty())
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), QStringLiteral("Missing experimentId"));
                return response;
            }
            QString errorMessage;
            if (!m_scopeonecore->cancelExperiment(experimentId, &errorMessage))
            {
                QJsonObject response = makeResponse(type, false);
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }
            return experimentStatusResponse(type, experimentId);
        }

        if (type == QStringLiteral("session_info"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const auto session = m_scopeonecore->recordingSession(sessionId);
            QJsonObject response = makeResponse(type, static_cast<bool>(session));
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
                return response;
            }

            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(session->recordedCameraIds()));
            response.insert(QStringLiteral("frameCount"), session->recordedFrameCount());
            QJsonObject counts;
            for (const QString& cameraId : session->recordedCameraIds())
            {
                counts.insert(cameraId, session->recordedFrameCount(cameraId));
            }
            response.insert(QStringLiteral("frameCounts"), counts);
            return response;
        }

        if (type == QStringLiteral("session_close"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const auto session = m_scopeonecore->recordingSession(sessionId);
            QJsonObject response = makeResponse(type, static_cast<bool>(session));
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
                return response;
            }

            m_scopeonecore->closeRecordingSession(sessionId);
            return response;
        }

        if (type == QStringLiteral("latest_raw_frame")
            || type == QStringLiteral("layer_frame"))
        {
            const bool rawFrameRequest = type == QStringLiteral("latest_raw_frame");
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            const QString layerKey = rawFrameRequest
                                         ? scopeone::core::ScopeOneCore::rawLayerKey(cameraId)
                                         : request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (layerKey.isEmpty())
            {
                response.insert(QStringLiteral("error"),
                                rawFrameRequest ? QStringLiteral("Missing camera")
                                                : QStringLiteral("Missing layerKey"));
                return response;
            }

            const scopeone::core::ImageFrame frame = m_scopeonecore->graphFrame(layerKey);
            if (!frame.isValid())
            {
                response.insert(QStringLiteral("error"),
                                rawFrameRequest ? QStringLiteral("No latest raw frame for camera")
                                                : QStringLiteral("Layer has no current frame"));
                return response;
            }

            QString errorMessage;
            scopeone::core::ImageFrame exportedFrame;
            if (!exportFrameToSharedMemory(frame, exportedFrame, errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? (rawFrameRequest
                                           ? QStringLiteral("Failed to export latest raw frame")
                                           : QStringLiteral("Failed to export layer frame"))
                                    : errorMessage);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            response.insert(QStringLiteral("layerKey"), layerKey);
            addFrameMetadata(response, exportedFrame);
            return response;
        }

        if (type == QStringLiteral("show_frame_mapping_as_layer"))
        {
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            const QString layerId = request.value(QStringLiteral("layerId"))
                                        .toString(QStringLiteral("python_result"))
                                        .trimmed();
            const QString displayName = request.value(QStringLiteral("name"))
                                            .toString(QStringLiteral("Python Result"))
                                            .trimmed();
            QJsonObject response = makeResponse(type, false);
            if (layerId.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing layerId"));
                return response;
            }

            scopeone::core::ImageFrame frame;
            QString errorMessage;
            if (!importFrameFromSharedMemory(cameraId, frame, errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to import frame")
                                    : errorMessage);
                return response;
            }

            const scopeone::core::ImageFrame graphFrame =
                publishExternalApiFrame(m_scopeonecore, frame, errorMessage);
            if (!graphFrame.isValid())
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }

            const scopeone::core::ImageFrame previewFrame =
                m_scopeonecore->publishStaticFrame(layerId, graphFrame, displayName);
            if (!previewFrame.isValid())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to show frame as preview layer"));
                return response;
            }
            const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(layerId);

            response = makeResponse(type, true);
            response.insert(QStringLiteral("layerKey"), layerKey);
            addFrameMetadata(response, graphFrame);
            return response;
        }

        QJsonObject response = makeResponse(type, false);
        response.insert(QStringLiteral("error"), QStringLiteral("Unknown request type"));
        return response;
    }

    // Returns the shared document or initializes a new draft
    scopeone::core::ExperimentDocument ScopeOneLocalApiServer::createExperimentDocument()
    {
        const scopeone::core::ExperimentDocument currentDocument = m_sceneModel->document();
        if (scopeone::core::validateExperimentDocument(currentDocument)
            && (currentDocument.runState == scopeone::core::ExperimentRunState::Draft
                || currentDocument.runState == scopeone::core::ExperimentRunState::Running))
        {
            return currentDocument;
        }

        scopeone::core::ExperimentDocument document;
        document.plan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        document.plan.cameraIds = m_scopeonecore->cameraIds();
        document.plan.streamToDisk = false;
        document.plan.configPath = m_scopeonecore->loadedConfigurationPath();
        document.plan.configSha256 = m_scopeonecore->loadedConfigurationSha256();
        document.plan.processing = m_scopeonecore->processingRecipe();

        copyValidPresentation(currentDocument, document);
        m_sceneModel->setDocument(document);
        return document;
    }

    // Builds one experiment status response from retained run state
    QJsonObject ScopeOneLocalApiServer::experimentStatusResponse(
        const QString& type,
        const QString& experimentId) const
    {
        QJsonObject response = makeResponse(type, false);
        if (experimentId.isEmpty())
        {
            response.insert(QStringLiteral("error"), QStringLiteral("Missing experimentId"));
            return response;
        }

        scopeone::core::ExperimentDocument experiment;
        if (!m_scopeonecore->experimentDocument(experimentId, experiment))
        {
            response.insert(QStringLiteral("error"), QStringLiteral("Unknown experiment"));
            return response;
        }

        response = makeResponse(type, true);
        response.insert(QStringLiteral("experimentId"), experimentId);
        response.insert(QStringLiteral("state"),
                        scopeone::core::experimentRunStateName(experiment.runState));
        response.insert(QStringLiteral("cancelRequested"),
                        m_scopeonecore->activeExperimentId() == experimentId
                            && m_scopeonecore->experimentCancelRequested());
        response.insert(QStringLiteral("document"),
                        scopeone::core::experimentDocumentToJson(experiment));

        if (m_scopeonecore->activeExperimentId() == experimentId)
        {
            response.insert(QStringLiteral("progress"),
                            recordingProgressToJson(m_scopeonecore->recordingProgress()));
            response.insert(QStringLiteral("writer"),
                            recordingWriterStatusToJson(m_scopeonecore->recordingWriterStatus()));
        }

        const auto session = m_scopeonecore->recordingSession(experimentId);
        if (session)
        {
            response.insert(QStringLiteral("sessionId"), experimentId);
            response.insert(QStringLiteral("cameraIds"),
                            QJsonArray::fromStringList(session->recordedCameraIds()));
            response.insert(QStringLiteral("frameCount"), session->recordedFrameCount());
        }
        return response;
    }

    // Exports one captured frame to the shared frame mapping
    bool ScopeOneLocalApiServer::exportFrameToSharedMemory(
        const scopeone::core::ImageFrame& frame,
        scopeone::core::ImageFrame& exportedFrame,
        QString& errorMessage)
    {
        exportedFrame = {};
#if defined(_WIN32)
        if (!m_frameMappingHandle || !m_frameMappingView)
#else
        if (!m_frameMappingView)
#endif
        {
            errorMessage = QStringLiteral("Frame mapping is not available");
            return false;
        }
        const qint64 payloadBytes = frame.payloadByteCount();
        if (!frame.isValid()
            || payloadBytes <= 0
            || payloadBytes > scopeone::core::kSharedFrameMaxBytes)
        {
            errorMessage = QStringLiteral("Frame payload is invalid");
            return false;
        }

        scopeone::core::SharedFrameHeader header = frame.toSharedFrameHeader();
        exportedFrame = frame;
        exportedFrame.cameraId = frame.cameraId.trimmed();
        exportedFrame.timestampNs = header.timestampNs;
        header.state = 1;
        std::memcpy(m_frameMappingView, &header, sizeof(header));
        std::memcpy(m_frameMappingView + scopeone::core::kSharedFrameHeaderSize,
                    frame.bytes.constData(),
                    static_cast<size_t>(payloadBytes));
        header.state = 2;
        std::memcpy(m_frameMappingView, &header, sizeof(header));
        m_frameMappingCameraId = frame.cameraId.trimmed();
        return true;
    }

    // Imports the current shared frame mapping as an ImageFrame
    bool ScopeOneLocalApiServer::importFrameFromSharedMemory(const QString& cameraId,
                                                             scopeone::core::ImageFrame& frame,
                                                             QString& errorMessage)
    {
#if defined(_WIN32)
        if (!m_frameMappingHandle || !m_frameMappingView)
#else
        if (!m_frameMappingView)
#endif
        {
            errorMessage = QStringLiteral("Frame mapping is not available");
            return false;
        }

        scopeone::core::SharedFrameHeader header{};
        std::memcpy(&header, m_frameMappingView, sizeof(header));
        if (header.state != 2)
        {
            errorMessage = QStringLiteral("Frame mapping does not contain a ready frame");
            return false;
        }

        const quint64 payloadBytes = static_cast<quint64>(header.stride) * header.height;
        if (payloadBytes == 0
            || payloadBytes > scopeone::core::kSharedFrameMaxBytes
            || payloadBytes > static_cast<quint64>((std::numeric_limits<qsizetype>::max)()))
        {
            errorMessage = QStringLiteral("Frame mapping payload is invalid");
            return false;
        }

        QByteArray payload;
        payload.resize(static_cast<qsizetype>(payloadBytes));
        std::memcpy(payload.data(),
                    m_frameMappingView + scopeone::core::kSharedFrameHeaderSize,
                    static_cast<size_t>(payloadBytes));

        const QString resolvedCameraId = cameraId.trimmed().isEmpty()
                                             ? m_frameMappingCameraId
                                             : cameraId.trimmed();
        frame = scopeone::core::ImageFrame::fromSharedFrame(resolvedCameraId, header, payload);
        if (!frame.isValid())
        {
            errorMessage = QStringLiteral("Frame mapping metadata is invalid");
            return false;
        }
        m_frameMappingCameraId = frame.cameraId.trimmed();
        return true;
    }
} // namespace scopeone::ui
