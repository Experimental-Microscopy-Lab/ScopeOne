#include "scopeone/ExperimentDocument.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaType>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>

namespace scopeone::core
{
    namespace
    {
        constexpr quint64 kMaximumJsonEventCount = static_cast<quint64>((std::numeric_limits<int>::max)());

        bool fail(QString* errorMessage, const QString& message)
        {
            if (errorMessage)
            {
                *errorMessage = message;
            }
            return false;
        }

        void clearError(QString* errorMessage)
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
        }

        QString memberPath(const QString& path, const QString& member)
        {
            return path.isEmpty() ? member : path + QLatin1Char('.') + member;
        }

        QString elementPath(const QString& path, qsizetype index)
        {
            return QStringLiteral("%1[%2]").arg(path).arg(index);
        }

        bool isFinite(double value)
        {
            return std::isfinite(value);
        }

        bool validateIdentifier(const QString& id, const QString& path, QString* errorMessage)
        {
            if (id.isEmpty())
            {
                return fail(errorMessage, QStringLiteral("%1 must not be empty").arg(path));
            }
            if (id.trimmed() != id)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must not contain leading or trailing whitespace").arg(path));
            }
            for (const QChar character : id)
            {
                if (!character.isPrint())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 contains a non-printable character").arg(path));
                }
            }
            return true;
        }

        bool validateFileNameComponent(const QString& name,
                                       const QString& path,
                                       QString* errorMessage)
        {
            if (name.isEmpty())
            {
                return true;
            }
            if (!validateIdentifier(name, path, errorMessage))
            {
                return false;
            }
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a file name, not a directory reference").arg(path));
            }

            static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
            for (const QChar character : name)
            {
                if (invalidCharacters.contains(character))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 contains a character that is not allowed in a file name")
                                    .arg(path));
                }
            }
            if (name.endsWith(QLatin1Char('.')))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must not end with a period").arg(path));
            }
            return true;
        }

        bool checkObjectFields(const QJsonObject& object,
                               std::initializer_list<QString> fields,
                               const QString& path,
                               QString* errorMessage)
        {
            QSet<QString> expected;
            for (const QString& field : fields)
            {
                expected.insert(field);
            }

            for (const QString& key : object.keys())
            {
                if (!expected.contains(key))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 contains unknown field '%2'").arg(path, key));
                }
            }

            for (const QString& field : fields)
            {
                if (!object.contains(field))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 is missing required field '%2'").arg(path, field));
                }
            }
            return true;
        }

        bool readObject(const QJsonObject& parent,
                        const QString& key,
                        QJsonObject& value,
                        const QString& path,
                        QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isObject())
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be an object").arg(memberPath(path, key)));
            }
            value = jsonValue.toObject();
            return true;
        }

        bool readArray(const QJsonObject& parent,
                       const QString& key,
                       QJsonArray& value,
                       const QString& path,
                       QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isArray())
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be an array").arg(memberPath(path, key)));
            }
            value = jsonValue.toArray();
            return true;
        }

        bool readString(const QJsonObject& parent,
                        const QString& key,
                        QString& value,
                        const QString& path,
                        QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isString())
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a string").arg(memberPath(path, key)));
            }
            value = jsonValue.toString();
            return true;
        }

        bool readBool(const QJsonObject& parent,
                      const QString& key,
                      bool& value,
                      const QString& path,
                      QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isBool())
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a boolean").arg(memberPath(path, key)));
            }
            value = jsonValue.toBool();
            return true;
        }

        bool readInt(const QJsonObject& parent,
                     const QString& key,
                     int& value,
                     const QString& path,
                     QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isDouble())
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be an integer").arg(memberPath(path, key)));
            }

            const double number = jsonValue.toDouble();
            if (!isFinite(number)
                || std::trunc(number) != number
                || number < static_cast<double>((std::numeric_limits<int>::min)())
                || number > static_cast<double>((std::numeric_limits<int>::max)()))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be an integer in the supported range")
                                .arg(memberPath(path, key)));
            }
            value = static_cast<int>(number);
            return true;
        }

        bool readDouble(const QJsonObject& parent,
                        const QString& key,
                        double& value,
                        const QString& path,
                        QString* errorMessage)
        {
            const QJsonValue jsonValue = parent.value(key);
            if (!jsonValue.isDouble() || !isFinite(jsonValue.toDouble()))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a finite number").arg(memberPath(path, key)));
            }
            value = jsonValue.toDouble();
            return true;
        }

        bool readUnsignedIntegerString(const QJsonObject& parent,
                                       const QString& key,
                                       quint64& value,
                                       const QString& path,
                                       QString* errorMessage)
        {
            QString text;
            if (!readString(parent, key, text, path, errorMessage))
            {
                return false;
            }
            bool ok = false;
            const quint64 parsed = text.toULongLong(&ok, 10);
            if (!ok || QString::number(parsed) != text)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a canonical unsigned decimal string")
                                .arg(memberPath(path, key)));
            }
            value = parsed;
            return true;
        }

        bool readSignedIntegerString(const QJsonObject& parent,
                                     const QString& key,
                                     qint64& value,
                                     const QString& path,
                                     QString* errorMessage)
        {
            QString text;
            if (!readString(parent, key, text, path, errorMessage))
            {
                return false;
            }
            bool ok = false;
            const qint64 parsed = text.toLongLong(&ok, 10);
            if (!ok || QString::number(parsed) != text)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must be a canonical signed decimal string")
                                .arg(memberPath(path, key)));
            }
            value = parsed;
            return true;
        }

        bool validateJsonValue(const QJsonValue& value, const QString& path, QString* errorMessage)
        {
            if (value.isUndefined())
            {
                return fail(errorMessage, QStringLiteral("%1 must not be undefined").arg(path));
            }
            if (value.isDouble() && !isFinite(value.toDouble()))
            {
                return fail(errorMessage, QStringLiteral("%1 must be finite").arg(path));
            }
            if (value.isArray())
            {
                const QJsonArray array = value.toArray();
                for (qsizetype index = 0; index < array.size(); ++index)
                {
                    if (!validateJsonValue(array.at(index), elementPath(path, index), errorMessage))
                    {
                        return false;
                    }
                }
            }
            else if (value.isObject())
            {
                const QJsonObject object = value.toObject();
                for (auto it = object.constBegin(); it != object.constEnd(); ++it)
                {
                    if (!validateJsonValue(it.value(), memberPath(path, it.key()), errorMessage))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        QJsonValue canonicalJsonValue(const QJsonValue& value)
        {
            if (value.isArray())
            {
                QJsonArray result;
                const QJsonArray array = value.toArray();
                for (const QJsonValue& item : array)
                {
                    result.append(canonicalJsonValue(item));
                }
                return result;
            }
            if (value.isObject())
            {
                QJsonObject result;
                const QJsonObject object = value.toObject();
                QStringList keys = object.keys();
                keys.sort(Qt::CaseSensitive);
                for (const QString& key : keys)
                {
                    result.insert(key, canonicalJsonValue(object.value(key)));
                }
                return result;
            }
            return value;
        }

        QJsonObject canonicalJsonObject(const QJsonObject& object)
        {
            return canonicalJsonValue(object).toObject();
        }

        bool validateVariant(const QVariant& value, const QString& path, QString* errorMessage)
        {
            if (!value.isValid() || value.isNull())
            {
                return true;
            }

            const int typeId = value.metaType().id();
            switch (typeId)
            {
            case QMetaType::Bool:
            case QMetaType::Char:
            case QMetaType::SChar:
            case QMetaType::UChar:
            case QMetaType::Short:
            case QMetaType::UShort:
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::Long:
            case QMetaType::ULong:
            case QMetaType::LongLong:
            case QMetaType::QString:
                return true;
            case QMetaType::ULongLong:
                if (value.toULongLong() > static_cast<quint64>((std::numeric_limits<qint64>::max)()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 exceeds the largest lossless JSON integer").arg(path));
                }
                return true;
            case QMetaType::Float:
            case QMetaType::Double:
                if (!isFinite(value.toDouble()))
                {
                    return fail(errorMessage, QStringLiteral("%1 must be finite").arg(path));
                }
                return true;
            case QMetaType::QStringList:
                return true;
            case QMetaType::QVariantList:
            {
                const QVariantList list = value.toList();
                for (qsizetype index = 0; index < list.size(); ++index)
                {
                    if (!validateVariant(list.at(index), elementPath(path, index), errorMessage))
                    {
                        return false;
                    }
                }
                return true;
            }
            case QMetaType::QVariantMap:
            {
                const QVariantMap map = value.toMap();
                for (auto it = map.constBegin(); it != map.constEnd(); ++it)
                {
                    if (!validateVariant(it.value(), memberPath(path, it.key()), errorMessage))
                    {
                        return false;
                    }
                }
                return true;
            }
            case QMetaType::QJsonValue:
                return validateJsonValue(value.value<QJsonValue>(), path, errorMessage);
            case QMetaType::QJsonArray:
                return validateJsonValue(value.value<QJsonArray>(), path, errorMessage);
            case QMetaType::QJsonObject:
                return validateJsonValue(value.value<QJsonObject>(), path, errorMessage);
            default:
                return fail(errorMessage,
                            QStringLiteral("%1 has unsupported QVariant type '%2'")
                                .arg(path, QString::fromLatin1(value.metaType().name())));
            }
        }

        bool validateVariantMap(const QVariantMap& map, const QString& path, QString* errorMessage)
        {
            for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            {
                if (!validateVariant(it.value(), memberPath(path, it.key()), errorMessage))
                {
                    return false;
                }
            }
            return true;
        }

        QJsonValue variantToJson(const QVariant& value)
        {
            if (!value.isValid() || value.isNull())
            {
                return QJsonValue(QJsonValue::Null);
            }

            switch (value.metaType().id())
            {
            case QMetaType::Bool:
                return value.toBool();
            case QMetaType::Char:
            case QMetaType::SChar:
            case QMetaType::Short:
            case QMetaType::Int:
            case QMetaType::Long:
            case QMetaType::LongLong:
                return QJsonValue(value.toLongLong());
            case QMetaType::UChar:
            case QMetaType::UShort:
            case QMetaType::UInt:
            case QMetaType::ULong:
            case QMetaType::ULongLong:
            {
                const quint64 number = value.toULongLong();
                if (number <= static_cast<quint64>((std::numeric_limits<qint64>::max)()))
                {
                    return QJsonValue(static_cast<qint64>(number));
                }
                return QString::number(number);
            }
            case QMetaType::Float:
            case QMetaType::Double:
                return value.toDouble();
            case QMetaType::QString:
                return value.toString();
            case QMetaType::QStringList:
                return QJsonArray::fromStringList(value.toStringList());
            case QMetaType::QVariantList:
            {
                QJsonArray result;
                for (const QVariant& item : value.toList())
                {
                    result.append(variantToJson(item));
                }
                return result;
            }
            case QMetaType::QVariantMap:
            {
                QJsonObject result;
                const QVariantMap map = value.toMap();
                for (auto it = map.constBegin(); it != map.constEnd(); ++it)
                {
                    result.insert(it.key(), variantToJson(it.value()));
                }
                return canonicalJsonObject(result);
            }
            case QMetaType::QJsonValue:
                return canonicalJsonValue(value.value<QJsonValue>());
            case QMetaType::QJsonArray:
                return canonicalJsonValue(value.value<QJsonArray>());
            case QMetaType::QJsonObject:
                return canonicalJsonValue(value.value<QJsonObject>());
            default:
                return QJsonValue::fromVariant(value);
            }
        }

        QJsonObject variantMapToJson(const QVariantMap& map)
        {
            QJsonObject result;
            for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            {
                result.insert(it.key(), variantToJson(it.value()));
            }
            return canonicalJsonObject(result);
        }

        QVariant jsonToVariant(const QJsonValue& value)
        {
            if (value.isArray())
            {
                QVariantList result;
                const QJsonArray array = value.toArray();
                result.reserve(array.size());
                for (const QJsonValue& item : array)
                {
                    result.append(jsonToVariant(item));
                }
                return result;
            }
            if (value.isObject())
            {
                QVariantMap result;
                const QJsonObject object = value.toObject();
                for (auto it = object.constBegin(); it != object.constEnd(); ++it)
                {
                    result.insert(it.key(), jsonToVariant(it.value()));
                }
                return result;
            }
            return value.toVariant();
        }

        QVariantMap variantMapFromJson(const QJsonObject& object)
        {
            QVariantMap result;
            for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            {
                result.insert(it.key(), jsonToVariant(it.value()));
            }
            return result;
        }

        QVariantMap processingParametersFromJson(const QString& moduleId,
                                                 const QJsonObject& object)
        {
            QVariantMap result = variantMapFromJson(object);
            const auto normalizeInt = [&object, &result](const QString& key)
            {
                const QJsonValue value = object.value(key);
                const double number = value.toDouble();
                if (value.isDouble()
                    && isFinite(number)
                    && std::trunc(number) == number
                    && number >= static_cast<double>((std::numeric_limits<int>::min)())
                    && number <= static_cast<double>((std::numeric_limits<int>::max)()))
                {
                    result.insert(key, static_cast<int>(number));
                }
            };
            const auto normalizeDouble = [&object, &result](const QString& key)
            {
                const QJsonValue value = object.value(key);
                if (value.isDouble() && isFinite(value.toDouble()))
                {
                    result.insert(key, value.toDouble());
                }
            };

            if (moduleId == QStringLiteral("frequency_domain_filter"))
            {
                normalizeDouble(QStringLiteral("min_feature_size"));
                normalizeDouble(QStringLiteral("max_feature_size"));
                normalizeInt(QStringLiteral("filter_kind"));
                normalizeInt(QStringLiteral("output_mode"));
            }
            else if (moduleId == QStringLiteral("background_calibration"))
            {
                normalizeInt(QStringLiteral("calibration_frames"));
                normalizeInt(QStringLiteral("operation"));
                normalizeInt(QStringLiteral("method"));
                normalizeInt(QStringLiteral("mode"));
            }
            else if (moduleId == QStringLiteral("spatiotemporal_binning"))
            {
                normalizeInt(QStringLiteral("spatial_bin_x"));
                normalizeInt(QStringLiteral("spatial_bin_y"));
                normalizeInt(QStringLiteral("temporal_bin"));
                normalizeInt(QStringLiteral("spatial_mode"));
                normalizeInt(QStringLiteral("temporal_mode"));
            }
            else if (moduleId == QStringLiteral("gaussian_blur"))
            {
                normalizeInt(QStringLiteral("kernel_size"));
                normalizeDouble(QStringLiteral("sigma"));
            }
            else if (moduleId == QStringLiteral("differential_rolling"))
            {
                normalizeInt(QStringLiteral("batch_size"));
            }
            return result;
        }

        QString recordingFormatName(RecordingFormat format)
        {
            switch (format)
            {
            case RecordingFormat::OmeTiff:
                return QStringLiteral("OmeTiff");
            case RecordingFormat::OmeZarr:
                return QStringLiteral("OmeZarr");
            case RecordingFormat::Tiff:
                return QStringLiteral("Tiff");
            case RecordingFormat::Binary:
                return QStringLiteral("Binary");
            }
            return QStringLiteral("Unknown");
        }

        QString imagePixelFormatName(ImagePixelFormat format)
        {
            switch (format)
            {
            case ImagePixelFormat::Invalid:
                return QStringLiteral("Invalid");
            case ImagePixelFormat::Mono8:
                return QStringLiteral("Mono8");
            case ImagePixelFormat::Mono16:
                return QStringLiteral("Mono16");
            }
            return QStringLiteral("Unknown");
        }

        bool parseRecordingFormat(const QString& name, RecordingFormat& format)
        {
            if (name == QStringLiteral("OmeTiff"))
            {
                format = RecordingFormat::OmeTiff;
                return true;
            }
            if (name == QStringLiteral("OmeZarr"))
            {
                format = RecordingFormat::OmeZarr;
                return true;
            }
            if (name == QStringLiteral("Tiff"))
            {
                format = RecordingFormat::Tiff;
                return true;
            }
            if (name == QStringLiteral("Binary"))
            {
                format = RecordingFormat::Binary;
                return true;
            }
            return false;
        }

        bool parseRecordingAxis(const QString& name, RecordingAxis& axis)
        {
            if (name == QStringLiteral("Time"))
            {
                axis = RecordingAxis::Time;
                return true;
            }
            if (name == QStringLiteral("Z"))
            {
                axis = RecordingAxis::Z;
                return true;
            }
            if (name == QStringLiteral("XY"))
            {
                axis = RecordingAxis::XY;
                return true;
            }
            return false;
        }

        QString processingModuleIdFromDocument(const QString& name)
        {
            if (name == QStringLiteral("FFT"))
            {
                return QStringLiteral("fft");
            }
            if (name == QStringLiteral("BackgroundCalibration"))
            {
                return QStringLiteral("background_calibration");
            }
            if (name == QStringLiteral("SpatiotemporalBinning"))
            {
                return QStringLiteral("spatiotemporal_binning");
            }
            if (name == QStringLiteral("GaussianBlur"))
            {
                return QStringLiteral("gaussian_blur");
            }
            if (name == QStringLiteral("DifferentialRolling"))
            {
                return QStringLiteral("differential_rolling");
            }
            return name == QStringLiteral("Unknown") ? QString{} : name.trimmed();
        }

        bool parseExperimentRunState(const QString& name, ExperimentRunState& state)
        {
            if (name == QStringLiteral("Draft"))
            {
                state = ExperimentRunState::Draft;
                return true;
            }
            if (name == QStringLiteral("Running"))
            {
                state = ExperimentRunState::Running;
                return true;
            }
            if (name == QStringLiteral("Completed"))
            {
                state = ExperimentRunState::Completed;
                return true;
            }
            if (name == QStringLiteral("Canceled"))
            {
                state = ExperimentRunState::Canceled;
                return true;
            }
            if (name == QStringLiteral("Failed"))
            {
                state = ExperimentRunState::Failed;
                return true;
            }
            return false;
        }

        bool parseDocumentLayerKind(const QString& name, DocumentLayerKind& kind)
        {
            if (name == QStringLiteral("Raw"))
            {
                kind = DocumentLayerKind::Raw;
                return true;
            }
            if (name == QStringLiteral("Processed"))
            {
                kind = DocumentLayerKind::Processed;
                return true;
            }
            if (name == QStringLiteral("Static"))
            {
                kind = DocumentLayerKind::Static;
                return true;
            }
            if (name == QStringLiteral("Tool"))
            {
                kind = DocumentLayerKind::Tool;
                return true;
            }
            if (name == QStringLiteral("Gallery"))
            {
                kind = DocumentLayerKind::Gallery;
                return true;
            }
            return false;
        }

        bool parseDocumentMarkupType(const QString& name, DocumentMarkupType& type)
        {
            if (name == QStringLiteral("Line"))
            {
                type = DocumentMarkupType::Line;
                return true;
            }
            if (name == QStringLiteral("Rect"))
            {
                type = DocumentMarkupType::Rect;
                return true;
            }
            return false;
        }

        bool parseDocumentMarkupRole(const QString& name, DocumentMarkupRole& role)
        {
            if (name == QStringLiteral("Generic"))
            {
                role = DocumentMarkupRole::Generic;
                return true;
            }
            if (name == QStringLiteral("CrossSection"))
            {
                role = DocumentMarkupRole::CrossSection;
                return true;
            }
            if (name == QStringLiteral("Roi"))
            {
                role = DocumentMarkupRole::Roi;
                return true;
            }
            if (name == QStringLiteral("Measurement"))
            {
                role = DocumentMarkupRole::Measurement;
                return true;
            }
            return false;
        }

        bool parseImagePixelFormat(const QString& name, ImagePixelFormat& format)
        {
            if (name == QStringLiteral("Invalid"))
            {
                format = ImagePixelFormat::Invalid;
                return true;
            }
            if (name == QStringLiteral("Mono8"))
            {
                format = ImagePixelFormat::Mono8;
                return true;
            }
            if (name == QStringLiteral("Mono16"))
            {
                format = ImagePixelFormat::Mono16;
                return true;
            }
            return false;
        }

        bool perBurstEventCount(const ExperimentPlan& plan, quint64& count, QString* errorMessage, const QString& path)
        {
            const quint64 zCount = plan.zPositions.empty() ? 1u : static_cast<quint64>(plan.zPositions.size());
            const quint64 positionCount = plan.positions.empty() ? 1u : static_cast<quint64>(plan.positions.size());
            const quint64 timeCount = static_cast<quint64>(plan.framesPerBurst);
            if (timeCount != 0 && zCount > kMaximumJsonEventCount / timeCount)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 expands to too many acquisition events").arg(path));
            }
            const quint64 timeAndZ = timeCount * zCount;
            if (timeAndZ != 0 && positionCount > kMaximumJsonEventCount / timeAndZ)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 expands to too many acquisition events").arg(path));
            }
            count = timeAndZ * positionCount;
            return true;
        }

        bool validateExperimentPlanImpl(const ExperimentPlan& plan,
                                        const QString& path,
                                        QString* errorMessage)
        {
            if (plan.schemaVersion != kExperimentDocumentSchemaVersion)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.schemaVersion %2 is unsupported; expected %3")
                                .arg(path)
                                .arg(plan.schemaVersion)
                                .arg(kExperimentDocumentSchemaVersion));
            }
            if (!validateIdentifier(plan.experimentId, memberPath(path, QStringLiteral("experimentId")), errorMessage))
            {
                return false;
            }
            if (plan.cameraIds.isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.cameraIds must contain at least one camera ID").arg(path));
            }

            QSet<QString> cameraIds;
            for (qsizetype index = 0; index < plan.cameraIds.size(); ++index)
            {
                const QString cameraPath = elementPath(memberPath(path, QStringLiteral("cameraIds")), index);
                const QString& cameraId = plan.cameraIds.at(index);
                if (!validateIdentifier(cameraId, cameraPath, errorMessage))
                {
                    return false;
                }
                if (!validateFileNameComponent(cameraId, cameraPath, errorMessage))
                {
                    return false;
                }
                if (cameraIds.contains(cameraId))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 duplicates camera ID '%2'").arg(cameraPath, cameraId));
                }
                cameraIds.insert(cameraId);
            }

            if (plan.format != RecordingFormat::OmeTiff
                && plan.format != RecordingFormat::OmeZarr
                && plan.format != RecordingFormat::Tiff
                && plan.format != RecordingFormat::Binary)
            {
                return fail(errorMessage, QStringLiteral("%1.format is invalid").arg(path));
            }
            if (plan.compressionLevel < 0 || plan.compressionLevel > 9)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.compressionLevel must be between 0 and 9").arg(path));
            }
            if (plan.enableCompression
                && plan.format != RecordingFormat::OmeTiff
                && plan.format != RecordingFormat::OmeZarr
                && plan.format != RecordingFormat::Tiff)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.enableCompression is only supported for OME-Zarr or TIFF-based recording").arg(path));
            }
            if (plan.framesPerBurst < 1)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.framesPerBurst must be at least 1").arg(path));
            }
            if (plan.targetBursts < 1)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.targetBursts must be at least 1").arg(path));
            }
            if (!plan.burstMode && plan.targetBursts != 1)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.targetBursts must be 1 when burstMode is false").arg(path));
            }
            if (!isFinite(plan.burstIntervalMs) || plan.burstIntervalMs < 0.0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.burstIntervalMs must be a finite non-negative number").arg(path));
            }
            if (!isFinite(plan.mdaIntervalMs) || plan.mdaIntervalMs < 0.0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.mdaIntervalMs must be a finite non-negative number").arg(path));
            }
            if (!isFinite(plan.exposureMs) || plan.exposureMs < 0.0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.exposureMs must be a finite non-negative number").arg(path));
            }
            if (!isFinite(plan.pixelSizeUm) || plan.pixelSizeUm < 0.0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.pixelSizeUm must be a finite non-negative number").arg(path));
            }

            if (plan.order.empty())
            {
                return fail(errorMessage, QStringLiteral("%1.order must not be empty").arg(path));
            }
            QSet<int> axes;
            for (size_t index = 0; index < plan.order.size(); ++index)
            {
                const RecordingAxis axis = plan.order.at(index);
                if (axis != RecordingAxis::Time && axis != RecordingAxis::Z && axis != RecordingAxis::XY)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.order[%2] is invalid").arg(path).arg(index));
                }
                const int axisValue = static_cast<int>(axis);
                if (axes.contains(axisValue))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.order[%2] duplicates axis '%3'")
                                    .arg(path)
                                    .arg(index)
                                    .arg(recordingAxisName(axis)));
                }
                axes.insert(axisValue);
            }
            if (!axes.contains(static_cast<int>(RecordingAxis::Time)))
            {
                return fail(errorMessage, QStringLiteral("%1.order must contain Time").arg(path));
            }
            if (plan.mdaIntervalMs > 0.0
                && plan.framesPerBurst > 1
                && plan.order.front() != RecordingAxis::Time)
            {
                return fail(errorMessage,
                            QStringLiteral(
                                "%1.order must place Time first as the outermost axis when mdaIntervalMs is greater than 0 and framesPerBurst is greater than 1")
                                .arg(path));
            }
            if (!plan.zPositions.empty() && !axes.contains(static_cast<int>(RecordingAxis::Z)))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.order must contain Z when zPositions is not empty").arg(path));
            }
            if (!plan.positions.empty() && !axes.contains(static_cast<int>(RecordingAxis::XY)))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.order must contain XY when positions is not empty").arg(path));
            }

            if (plan.positions.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return fail(errorMessage, QStringLiteral("%1.positions contains too many entries").arg(path));
            }
            for (size_t index = 0; index < plan.positions.size(); ++index)
            {
                const QPointF& position = plan.positions.at(index);
                if (!isFinite(position.x()) || !isFinite(position.y()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.positions[%2] must contain finite coordinates")
                                    .arg(path)
                                    .arg(index));
                }
            }
            if (plan.zPositions.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return fail(errorMessage, QStringLiteral("%1.zPositions contains too many entries").arg(path));
            }
            for (size_t index = 0; index < plan.zPositions.size(); ++index)
            {
                if (!isFinite(plan.zPositions.at(index)))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.zPositions[%2] must be finite").arg(path).arg(index));
                }
            }

            if (plan.streamToDisk && plan.saveDir.trimmed().isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.saveDir must not be empty when streamToDisk is true").arg(path));
            }
            if (plan.streamToDisk && plan.baseName.trimmed().isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.baseName must not be empty when streamToDisk is true").arg(path));
            }
            if (!validateFileNameComponent(plan.baseName,
                                           memberPath(path, QStringLiteral("baseName")),
                                           errorMessage)
                || !validateFileNameComponent(plan.metadataFileName,
                                               memberPath(path, QStringLiteral("metadataFileName")),
                                               errorMessage))
            {
                return false;
            }
            if (!plan.configSha256.isEmpty())
            {
                if (plan.configPath.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.configPath must not be empty when configSha256 is set").arg(path));
                }
                if (plan.configSha256.size() != 64)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.configSha256 must contain exactly 64 hexadecimal characters")
                                    .arg(path));
                }
                for (const QChar character : plan.configSha256)
                {
                    const ushort value = character.unicode();
                    const bool hexadecimal = (value >= '0' && value <= '9')
                        || (value >= 'a' && value <= 'f');
                    if (!hexadecimal)
                    {
                        return fail(errorMessage,
                                    QStringLiteral("%1.configSha256 must contain only hexadecimal characters")
                                        .arg(path));
                    }
                }
            }

            if (plan.processing.bitDepth != ProcessingBitDepth::Bit8
                && plan.processing.bitDepth != ProcessingBitDepth::Bit16)
            {
                return fail(errorMessage, QStringLiteral("%1.processing.bitDepth is invalid").arg(path));
            }
            for (qsizetype index = 0; index < plan.processing.modules.size(); ++index)
            {
                const ProcessingModuleRecipe& module = plan.processing.modules.at(index);
                const QString modulePath = elementPath(memberPath(memberPath(path, QStringLiteral("processing")),
                                                                  QStringLiteral("modules")),
                                                       index);
                if (module.schemaVersion <= 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.schemaVersion must be positive; got %2")
                                    .arg(modulePath)
                                    .arg(module.schemaVersion));
                }
                if (module.moduleId.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.kind must name a supported processing module").arg(modulePath));
                }
                if (!validateVariantMap(module.parameters,
                                        memberPath(modulePath, QStringLiteral("parameters")),
                                        errorMessage))
                {
                    return false;
                }
            }
            if (!validateJsonValue(plan.userMetadata,
                                   memberPath(path, QStringLiteral("userMetadata")),
                                   errorMessage))
            {
                return false;
            }

            quint64 eventsPerBurst = 0;
            if (!perBurstEventCount(plan, eventsPerBurst, errorMessage, path))
            {
                return false;
            }
            const quint64 burstCount = static_cast<quint64>(plan.burstMode ? plan.targetBursts : 1);
            if (eventsPerBurst != 0
                && burstCount > (std::numeric_limits<quint64>::max)() / eventsPerBurst)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 produces sequence indices outside the supported range").arg(path));
            }

            const long double lastTimeOffset = static_cast<long double>(burstCount - 1)
                    * static_cast<long double>(plan.burstIntervalMs)
                + static_cast<long double>(plan.framesPerBurst - 1)
                    * static_cast<long double>(plan.mdaIntervalMs);
            if (lastTimeOffset > static_cast<long double>((std::numeric_limits<qint64>::max)()))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 timing offsets exceed the supported millisecond range").arg(path));
            }
            return true;
        }

        bool validateFrameRecord(const FrameRecord& frame, const QString& path, QString* errorMessage)
        {
            if (!validateIdentifier(frame.cameraId, memberPath(path, QStringLiteral("cameraId")), errorMessage))
            {
                return false;
            }
            if (frame.width < 1 || frame.height < 1)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 width and height must both be positive").arg(path));
            }
            if (frame.pixelFormat != ImagePixelFormat::Mono8 && frame.pixelFormat != ImagePixelFormat::Mono16)
            {
                return fail(errorMessage, QStringLiteral("%1.pixelFormat is invalid").arg(path));
            }
            if (frame.bitsPerSample != ImageFrame::normalizedBitsPerSample(frame.pixelFormat,
                                                                           frame.bitsPerSample))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.bitsPerSample is incompatible with pixelFormat").arg(path));
            }

            const bool hasRoiWidth = frame.sourceRoiWidth > 0;
            const bool hasRoiHeight = frame.sourceRoiHeight > 0;
            if (hasRoiWidth != hasRoiHeight)
            {
                return fail(errorMessage,
                            QStringLiteral(
                                "%1 source ROI width and height must either both be positive or both be zero")
                                .arg(path));
            }
            if (hasRoiWidth)
            {
                if (frame.sourceRoiX < 0 || frame.sourceRoiY < 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 source ROI offsets must be non-negative").arg(path));
                }
            }
            else if (frame.sourceRoiX != 0 || frame.sourceRoiY != 0
                || frame.sourceRoiWidth != 0 || frame.sourceRoiHeight != 0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1 absent source ROI must use zero for all ROI fields").arg(path));
            }
            return true;
        }

        bool validateAcquisitionEvent(const AcquisitionEvent& event,
                                      const ExperimentPlan& plan,
                                      const QString& path,
                                      QString* errorMessage)
        {
            const int burstCount = plan.burstMode ? plan.targetBursts : 1;
            if (event.burstIndex < 0 || event.burstIndex >= burstCount)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.burstIndex must be between 0 and %2")
                                .arg(path)
                                .arg(burstCount - 1));
            }
            if (event.timeIndex < 0 || event.timeIndex >= plan.framesPerBurst)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.timeIndex must be between 0 and %2")
                                .arg(path)
                                .arg(plan.framesPerBurst - 1));
            }
            if (!isFinite(event.x) || !isFinite(event.y) || !isFinite(event.z))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 spatial coordinates must be finite").arg(path));
            }

            if (plan.positions.empty())
            {
                if (event.hasXY || event.positionIndex != 0 || event.x != 0.0 || event.y != 0.0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must use the empty XY sentinel for a plan without positions")
                                    .arg(path));
                }
            }
            else
            {
                if (!event.hasXY
                    || event.positionIndex < 0
                    || static_cast<size_t>(event.positionIndex) >= plan.positions.size())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.positionIndex and hasXY do not reference a plan position")
                                    .arg(path));
                }
                const QPointF& expected = plan.positions.at(static_cast<size_t>(event.positionIndex));
                if (event.x != expected.x() || event.y != expected.y())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 XY coordinates do not match plan.positions[%2]")
                                    .arg(path)
                                    .arg(event.positionIndex));
                }
            }

            if (plan.zPositions.empty())
            {
                if (event.hasZ || event.zIndex != 0 || event.z != 0.0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must use the empty Z sentinel for a plan without zPositions")
                                    .arg(path));
                }
            }
            else
            {
                if (!event.hasZ
                    || event.zIndex < 0
                    || static_cast<size_t>(event.zIndex) >= plan.zPositions.size())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.zIndex and hasZ do not reference a plan Z position")
                                    .arg(path));
                }
                if (event.z != plan.zPositions.at(static_cast<size_t>(event.zIndex)))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.z does not match plan.zPositions[%2]")
                                    .arg(path)
                                    .arg(event.zIndex));
                }
            }

            if (!isFinite(event.exposureMs) || event.exposureMs < 0.0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.exposureMs must be a finite non-negative number").arg(path));
            }
            if (event.minimumStartTimeMs < 0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.minimumStartTimeMs must be non-negative").arg(path));
            }
            if (event.cameraIds.isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.cameraIds must contain at least one camera ID").arg(path));
            }

            const QSet<QString> planCameraIds(plan.cameraIds.constBegin(), plan.cameraIds.constEnd());
            QSet<QString> eventCameraIds;
            for (qsizetype index = 0; index < event.cameraIds.size(); ++index)
            {
                const QString cameraPath = elementPath(memberPath(path, QStringLiteral("cameraIds")), index);
                const QString& cameraId = event.cameraIds.at(index);
                if (!validateIdentifier(cameraId, cameraPath, errorMessage))
                {
                    return false;
                }
                if (eventCameraIds.contains(cameraId))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 duplicates camera ID '%2'").arg(cameraPath, cameraId));
                }
                if (!planCameraIds.contains(cameraId))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 references camera ID '%2' that is not in the plan")
                                    .arg(cameraPath, cameraId));
                }
                eventCameraIds.insert(cameraId);
            }

            return true;
        }

        bool validateAcquisitionEventRecord(const AcquisitionEventRecord& record,
                                            const ExperimentPlan& plan,
                                            const QString& path,
                                            QString* errorMessage)
        {
            if (!validateAcquisitionEvent(record.event,
                                          plan,
                                          memberPath(path, QStringLiteral("event")),
                                          errorMessage))
            {
                return false;
            }
            if (record.completedTimestampNs != 0 && record.startedTimestampNs == 0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.completedTimestampNs requires a non-zero startedTimestampNs")
                                .arg(path));
            }
            if (record.completedTimestampNs != 0
                && record.completedTimestampNs < record.startedTimestampNs)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.completedTimestampNs must not precede startedTimestampNs")
                                .arg(path));
            }
            if (record.succeeded && record.completedTimestampNs == 0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.succeeded requires a completion timestamp").arg(path));
            }
            if (record.succeeded && !record.errorMessage.isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.errorMessage must be empty for a succeeded event").arg(path));
            }
            if (!record.succeeded
                && record.completedTimestampNs != 0
                && record.errorMessage.trimmed().isEmpty())
            {
                return fail(errorMessage,
                            QStringLiteral("%1.errorMessage must describe a completed failed event").arg(path));
            }

            const QSet<QString> eventCameraIds(record.event.cameraIds.constBegin(),
                                               record.event.cameraIds.constEnd());
            for (auto it = record.frames.constBegin(); it != record.frames.constEnd(); ++it)
            {
                const QString framePath = QStringLiteral("%1.frames[%2]").arg(path, it.key());
                if (!validateIdentifier(it.key(), framePath + QStringLiteral(" key"), errorMessage))
                {
                    return false;
                }
                if (!eventCameraIds.contains(it.key()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 uses camera ID '%2' that is not in event.cameraIds")
                                    .arg(framePath, it.key()));
                }
                if (it.value().cameraId != it.key())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.cameraId must match frame map key '%2'")
                                    .arg(framePath, it.key()));
                }
                if (!validateFrameRecord(it.value(), framePath, errorMessage))
                {
                    return false;
                }
            }
            if (record.succeeded)
            {
                for (const QString& cameraId : record.event.cameraIds)
                {
                    if (!record.frames.contains(cameraId))
                    {
                        return fail(errorMessage,
                                    QStringLiteral("%1.frames is missing succeeded camera ID '%2'")
                                        .arg(path, cameraId));
                    }
                }
            }
            return true;
        }

        bool validateTransform(const ImageTransform2D& transform,
                               const QString& path,
                               QString* errorMessage)
        {
            if (!isFinite(transform.m11) || !isFinite(transform.m12)
                || !isFinite(transform.m21) || !isFinite(transform.m22)
                || !isFinite(transform.dx) || !isFinite(transform.dy))
            {
                return fail(errorMessage,
                            QStringLiteral("%1 must contain only finite values").arg(path));
            }
            ImageTransform2D inverse;
            if (!transform.inverted(inverse))
            {
                return fail(errorMessage, QStringLiteral("%1 must be invertible").arg(path));
            }
            return true;
        }

        QJsonObject pointToJson(const QPointF& point)
        {
            QJsonObject object;
            object.insert(QStringLiteral("x"), point.x());
            object.insert(QStringLiteral("y"), point.y());
            return canonicalJsonObject(object);
        }

        QJsonObject rectToJson(const QRectF& rect)
        {
            QJsonObject object;
            object.insert(QStringLiteral("x"), rect.x());
            object.insert(QStringLiteral("y"), rect.y());
            object.insert(QStringLiteral("width"), rect.width());
            object.insert(QStringLiteral("height"), rect.height());
            return canonicalJsonObject(object);
        }

        QJsonObject transformToJson(const ImageTransform2D& transform)
        {
            QJsonObject object;
            object.insert(QStringLiteral("m11"), transform.m11);
            object.insert(QStringLiteral("m12"), transform.m12);
            object.insert(QStringLiteral("m21"), transform.m21);
            object.insert(QStringLiteral("m22"), transform.m22);
            object.insert(QStringLiteral("dx"), transform.dx);
            object.insert(QStringLiteral("dy"), transform.dy);
            return canonicalJsonObject(object);
        }

        QJsonObject displayStateToJson(const LayerDisplayState& display)
        {
            QJsonObject object;
            object.insert(QStringLiteral("visible"), display.visible);
            object.insert(QStringLiteral("opacityPercent"), display.opacityPercent);
            object.insert(QStringLiteral("gamma"), display.gamma);
            object.insert(QStringLiteral("colormap"), display.colormap);
            object.insert(QStringLiteral("blending"), display.blending);
            object.insert(QStringLiteral("levelMin"), display.levelMin);
            object.insert(QStringLiteral("levelMax"), display.levelMax);
            object.insert(QStringLiteral("levelDomainMax"), display.levelDomainMax);
            return canonicalJsonObject(object);
        }

        QJsonObject acquisitionEventToJson(const AcquisitionEvent& event)
        {
            QJsonObject object;
            object.insert(QStringLiteral("sequenceIndex"), QString::number(event.sequenceIndex));
            object.insert(QStringLiteral("burstIndex"), event.burstIndex);
            object.insert(QStringLiteral("timeIndex"), event.timeIndex);
            object.insert(QStringLiteral("zIndex"), event.zIndex);
            object.insert(QStringLiteral("positionIndex"), event.positionIndex);
            object.insert(QStringLiteral("x"), event.x);
            object.insert(QStringLiteral("y"), event.y);
            object.insert(QStringLiteral("z"), event.z);
            object.insert(QStringLiteral("hasXY"), event.hasXY);
            object.insert(QStringLiteral("hasZ"), event.hasZ);
            object.insert(QStringLiteral("exposureMs"), event.exposureMs);
            object.insert(QStringLiteral("minimumStartTimeMs"), QString::number(event.minimumStartTimeMs));
            object.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(event.cameraIds));
            return canonicalJsonObject(object);
        }

        QJsonObject frameRecordToJson(const FrameRecord& frame)
        {
            QJsonObject object;
            object.insert(QStringLiteral("cameraId"), frame.cameraId);
            object.insert(QStringLiteral("width"), frame.width);
            object.insert(QStringLiteral("height"), frame.height);
            object.insert(QStringLiteral("bitsPerSample"), frame.bitsPerSample);
            object.insert(QStringLiteral("pixelFormat"), imagePixelFormatName(frame.pixelFormat));
            object.insert(QStringLiteral("frameIndex"), QString::number(frame.frameIndex));
            object.insert(QStringLiteral("timestampNs"), QString::number(frame.timestampNs));
            object.insert(QStringLiteral("sourceRoiX"), frame.sourceRoiX);
            object.insert(QStringLiteral("sourceRoiY"), frame.sourceRoiY);
            object.insert(QStringLiteral("sourceRoiWidth"), frame.sourceRoiWidth);
            object.insert(QStringLiteral("sourceRoiHeight"), frame.sourceRoiHeight);
            return canonicalJsonObject(object);
        }

        QJsonObject eventRecordToJson(const AcquisitionEventRecord& record)
        {
            QJsonObject frames;
            QStringList cameraIds = record.frames.keys();
            cameraIds.sort(Qt::CaseSensitive);
            for (const QString& cameraId : cameraIds)
            {
                frames.insert(cameraId, frameRecordToJson(record.frames.value(cameraId)));
            }

            QJsonObject object;
            object.insert(QStringLiteral("event"), acquisitionEventToJson(record.event));
            object.insert(QStringLiteral("startedTimestampNs"), QString::number(record.startedTimestampNs));
            object.insert(QStringLiteral("completedTimestampNs"), QString::number(record.completedTimestampNs));
            object.insert(QStringLiteral("succeeded"), record.succeeded);
            object.insert(QStringLiteral("errorMessage"), record.errorMessage);
            object.insert(QStringLiteral("frames"), canonicalJsonObject(frames));
            return canonicalJsonObject(object);
        }

        QJsonObject softwareToJson(const SoftwareSnapshot& software)
        {
            QJsonObject object;
            object.insert(QStringLiteral("applicationVersion"), software.applicationVersion);
            object.insert(QStringLiteral("coreVersion"), software.coreVersion);
            object.insert(QStringLiteral("mmCoreVersion"), software.mmCoreVersion);
            object.insert(QStringLiteral("libTiffVersion"), software.libTiffVersion);
            object.insert(QStringLiteral("zlibVersion"), software.zlibVersion);
            object.insert(QStringLiteral("operatingSystem"), software.operatingSystem);
            return canonicalJsonObject(object);
        }

        QJsonObject outputToJson(const RecordingOutputManifest& output)
        {
            QJsonObject files;
            QStringList cameraIds = output.files.keys();
            cameraIds.sort(Qt::CaseSensitive);
            for (const QString& cameraId : cameraIds)
            {
                const RecordingFileManifest& file = output.files.value(cameraId);
                QJsonObject fileObject;
                fileObject.insert(QStringLiteral("rawPath"), file.rawPath);
                fileObject.insert(QStringLiteral("frameInfoPath"), file.frameInfoPath);
                fileObject.insert(QStringLiteral("framesWritten"), QString::number(file.framesWritten));
                files.insert(cameraId, canonicalJsonObject(fileObject));
            }

            QJsonObject object;
            object.insert(QStringLiteral("streamedToDisk"), output.streamedToDisk);
            object.insert(QStringLiteral("files"), canonicalJsonObject(files));
            return canonicalJsonObject(object);
        }

        QJsonObject layerToJson(const DocumentLayer& layer)
        {
            QJsonObject object;
            object.insert(QStringLiteral("id"), layer.id);
            object.insert(QStringLiteral("sourceId"), layer.sourceId);
            object.insert(QStringLiteral("name"), layer.name);
            object.insert(QStringLiteral("kind"), documentLayerKindName(layer.kind));
            object.insert(QStringLiteral("width"), layer.width);
            object.insert(QStringLiteral("height"), layer.height);
            object.insert(QStringLiteral("pixelToSensor"), transformToJson(layer.pixelToSensor));
            object.insert(QStringLiteral("display"), displayStateToJson(layer.display));
            return canonicalJsonObject(object);
        }

        QJsonObject markupToJson(const DocumentMarkup& markup)
        {
            QJsonObject object;
            object.insert(QStringLiteral("id"), markup.id);
            object.insert(QStringLiteral("type"), documentMarkupTypeName(markup.type));
            object.insert(QStringLiteral("role"), documentMarkupRoleName(markup.role));
            object.insert(QStringLiteral("layerId"), markup.layerId);
            object.insert(QStringLiteral("label"), markup.label);
            object.insert(QStringLiteral("start"), pointToJson(markup.start));
            object.insert(QStringLiteral("end"), pointToJson(markup.end));
            object.insert(QStringLiteral("rect"), rectToJson(markup.rect));
            object.insert(QStringLiteral("visible"), markup.visible);
            object.insert(QStringLiteral("selected"), markup.selected);
            return canonicalJsonObject(object);
        }

        QJsonObject experimentPlanToJsonImpl(const ExperimentPlan& plan)
        {
            QJsonArray order;
            for (const RecordingAxis axis : plan.order)
            {
                order.append(recordingAxisName(axis));
            }
            QJsonArray positions;
            for (const QPointF& position : plan.positions)
            {
                positions.append(pointToJson(position));
            }
            QJsonArray zPositions;
            for (const double z : plan.zPositions)
            {
                zPositions.append(z);
            }
            QJsonArray modules;
            for (const ProcessingModuleRecipe& module : plan.processing.modules)
            {
                QJsonObject moduleObject;
                moduleObject.insert(QStringLiteral("kind"), module.moduleId);
                moduleObject.insert(QStringLiteral("schemaVersion"), module.schemaVersion);
                moduleObject.insert(QStringLiteral("parameters"), variantMapToJson(module.parameters));
                modules.append(canonicalJsonObject(moduleObject));
            }
            QJsonObject processing;
            processing.insert(QStringLiteral("bitDepth"), static_cast<int>(plan.processing.bitDepth));
            processing.insert(QStringLiteral("modules"), modules);

            QJsonObject object;
            object.insert(QStringLiteral("schemaVersion"), plan.schemaVersion);
            object.insert(QStringLiteral("experimentId"), plan.experimentId);
            object.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(plan.cameraIds));
            object.insert(QStringLiteral("format"), recordingFormatName(plan.format));
            object.insert(QStringLiteral("streamToDisk"), plan.streamToDisk);
            object.insert(QStringLiteral("enableCompression"), plan.enableCompression);
            object.insert(QStringLiteral("compressionLevel"), plan.compressionLevel);
            object.insert(QStringLiteral("framesPerBurst"), plan.framesPerBurst);
            object.insert(QStringLiteral("burstMode"), plan.burstMode);
            object.insert(QStringLiteral("targetBursts"), plan.targetBursts);
            object.insert(QStringLiteral("burstIntervalMs"), plan.burstIntervalMs);
            object.insert(QStringLiteral("mdaIntervalMs"), plan.mdaIntervalMs);
            object.insert(QStringLiteral("exposureMs"), plan.exposureMs);
            object.insert(QStringLiteral("pixelSizeUm"), plan.pixelSizeUm);
            object.insert(QStringLiteral("order"), order);
            object.insert(QStringLiteral("positions"), positions);
            object.insert(QStringLiteral("zPositions"), zPositions);
            object.insert(QStringLiteral("saveDir"), plan.saveDir);
            object.insert(QStringLiteral("baseName"), plan.baseName);
            object.insert(QStringLiteral("metadataFileName"), plan.metadataFileName);
            object.insert(QStringLiteral("configPath"), plan.configPath);
            object.insert(QStringLiteral("configSha256"), plan.configSha256);
            object.insert(QStringLiteral("processing"), canonicalJsonObject(processing));
            object.insert(QStringLiteral("userMetadata"), canonicalJsonObject(plan.userMetadata));
            return canonicalJsonObject(object);
        }

        bool parsePoint(const QJsonObject& object,
                        QPointF& point,
                        const QString& path,
                        QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("x"), QStringLiteral("y")},
                                   path,
                                   errorMessage))
            {
                return false;
            }
            double x = 0.0;
            double y = 0.0;
            if (!readDouble(object, QStringLiteral("x"), x, path, errorMessage)
                || !readDouble(object, QStringLiteral("y"), y, path, errorMessage))
            {
                return false;
            }
            point = QPointF(x, y);
            return true;
        }

        bool parseRect(const QJsonObject& object,
                       QRectF& rect,
                       const QString& path,
                       QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("x"),
                                    QStringLiteral("y"),
                                    QStringLiteral("width"),
                                    QStringLiteral("height")},
                                   path,
                                   errorMessage))
            {
                return false;
            }
            double x = 0.0;
            double y = 0.0;
            double width = 0.0;
            double height = 0.0;
            if (!readDouble(object, QStringLiteral("x"), x, path, errorMessage)
                || !readDouble(object, QStringLiteral("y"), y, path, errorMessage)
                || !readDouble(object, QStringLiteral("width"), width, path, errorMessage)
                || !readDouble(object, QStringLiteral("height"), height, path, errorMessage))
            {
                return false;
            }
            rect = QRectF(x, y, width, height);
            return true;
        }

        bool parseTransform(const QJsonObject& object,
                            ImageTransform2D& transform,
                            const QString& path,
                            QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("m11"),
                                    QStringLiteral("m12"),
                                    QStringLiteral("m21"),
                                    QStringLiteral("m22"),
                                    QStringLiteral("dx"),
                                    QStringLiteral("dy")},
                                   path,
                                   errorMessage))
            {
                return false;
            }
            return readDouble(object, QStringLiteral("m11"), transform.m11, path, errorMessage)
                && readDouble(object, QStringLiteral("m12"), transform.m12, path, errorMessage)
                && readDouble(object, QStringLiteral("m21"), transform.m21, path, errorMessage)
                && readDouble(object, QStringLiteral("m22"), transform.m22, path, errorMessage)
                && readDouble(object, QStringLiteral("dx"), transform.dx, path, errorMessage)
                && readDouble(object, QStringLiteral("dy"), transform.dy, path, errorMessage);
        }

        bool parseDisplayState(const QJsonObject& object,
                               LayerDisplayState& display,
                               const QString& path,
                               QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("visible"),
                                    QStringLiteral("opacityPercent"),
                                    QStringLiteral("gamma"),
                                    QStringLiteral("colormap"),
                                    QStringLiteral("blending"),
                                    QStringLiteral("levelMin"),
                                    QStringLiteral("levelMax"),
                                    QStringLiteral("levelDomainMax")},
                                   path,
                                   errorMessage))
            {
                return false;
            }
            return readBool(object, QStringLiteral("visible"), display.visible, path, errorMessage)
                && readInt(object,
                           QStringLiteral("opacityPercent"),
                           display.opacityPercent,
                           path,
                           errorMessage)
                && readDouble(object, QStringLiteral("gamma"), display.gamma, path, errorMessage)
                && readString(object, QStringLiteral("colormap"), display.colormap, path, errorMessage)
                && readString(object, QStringLiteral("blending"), display.blending, path, errorMessage)
                && readInt(object, QStringLiteral("levelMin"), display.levelMin, path, errorMessage)
                && readInt(object, QStringLiteral("levelMax"), display.levelMax, path, errorMessage)
                && readInt(object,
                           QStringLiteral("levelDomainMax"),
                           display.levelDomainMax,
                           path,
                           errorMessage);
        }

        bool parseStringArray(const QJsonArray& array,
                              QStringList& strings,
                              const QString& path,
                              QString* errorMessage)
        {
            QStringList parsed;
            parsed.reserve(array.size());
            for (qsizetype index = 0; index < array.size(); ++index)
            {
                if (!array.at(index).isString())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must be a string").arg(elementPath(path, index)));
                }
                parsed.append(array.at(index).toString());
            }
            strings = parsed;
            return true;
        }

        bool parseExperimentPlanObject(const QJsonObject& object,
                                       ExperimentPlan& plan,
                                       const QString& path,
                                       QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("schemaVersion"),
                                    QStringLiteral("experimentId"),
                                    QStringLiteral("cameraIds"),
                                    QStringLiteral("format"),
                                    QStringLiteral("streamToDisk"),
                                    QStringLiteral("enableCompression"),
                                    QStringLiteral("compressionLevel"),
                                    QStringLiteral("framesPerBurst"),
                                    QStringLiteral("burstMode"),
                                    QStringLiteral("targetBursts"),
                                    QStringLiteral("burstIntervalMs"),
                                    QStringLiteral("mdaIntervalMs"),
                                    QStringLiteral("exposureMs"),
                                    QStringLiteral("pixelSizeUm"),
                                    QStringLiteral("order"),
                                    QStringLiteral("positions"),
                                    QStringLiteral("zPositions"),
                                    QStringLiteral("saveDir"),
                                    QStringLiteral("baseName"),
                                    QStringLiteral("metadataFileName"),
                                    QStringLiteral("configPath"),
                                    QStringLiteral("configSha256"),
                                    QStringLiteral("processing"),
                                    QStringLiteral("userMetadata")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            ExperimentPlan parsed;
            if (!readInt(object,
                         QStringLiteral("schemaVersion"),
                         parsed.schemaVersion,
                         path,
                         errorMessage))
            {
                return false;
            }
            if (parsed.schemaVersion != kExperimentDocumentSchemaVersion)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.schemaVersion %2 is unsupported; expected %3")
                                .arg(path)
                                .arg(parsed.schemaVersion)
                                .arg(kExperimentDocumentSchemaVersion));
            }

            QJsonArray cameraIds;
            QString formatName;
            if (!readString(object,
                            QStringLiteral("experimentId"),
                            parsed.experimentId,
                            path,
                            errorMessage)
                || !readArray(object, QStringLiteral("cameraIds"), cameraIds, path, errorMessage)
                || !parseStringArray(cameraIds,
                                     parsed.cameraIds,
                                     memberPath(path, QStringLiteral("cameraIds")),
                                     errorMessage)
                || !readString(object, QStringLiteral("format"), formatName, path, errorMessage))
            {
                return false;
            }
            if (!parseRecordingFormat(formatName, parsed.format))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.format must be 'OmeTiff', 'OmeZarr', 'Tiff' or 'Binary'").arg(path));
            }

            if (!readBool(object,
                          QStringLiteral("streamToDisk"),
                          parsed.streamToDisk,
                          path,
                          errorMessage)
                || !readBool(object,
                             QStringLiteral("enableCompression"),
                             parsed.enableCompression,
                             path,
                             errorMessage)
                || !readInt(object,
                            QStringLiteral("compressionLevel"),
                            parsed.compressionLevel,
                            path,
                            errorMessage)
                || !readInt(object,
                            QStringLiteral("framesPerBurst"),
                            parsed.framesPerBurst,
                            path,
                            errorMessage)
                || !readBool(object,
                             QStringLiteral("burstMode"),
                             parsed.burstMode,
                             path,
                             errorMessage)
                || !readInt(object,
                            QStringLiteral("targetBursts"),
                            parsed.targetBursts,
                            path,
                            errorMessage)
                || !readDouble(object,
                               QStringLiteral("burstIntervalMs"),
                               parsed.burstIntervalMs,
                               path,
                               errorMessage)
                || !readDouble(object,
                               QStringLiteral("mdaIntervalMs"),
                               parsed.mdaIntervalMs,
                               path,
                               errorMessage)
                || !readDouble(object,
                               QStringLiteral("exposureMs"),
                               parsed.exposureMs,
                               path,
                               errorMessage)
                || !readDouble(object,
                               QStringLiteral("pixelSizeUm"),
                               parsed.pixelSizeUm,
                               path,
                               errorMessage))
            {
                return false;
            }

            QJsonArray order;
            if (!readArray(object, QStringLiteral("order"), order, path, errorMessage))
            {
                return false;
            }
            parsed.order.clear();
            parsed.order.reserve(static_cast<size_t>(order.size()));
            for (qsizetype index = 0; index < order.size(); ++index)
            {
                if (!order.at(index).isString())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must be an axis name")
                                    .arg(elementPath(memberPath(path, QStringLiteral("order")), index)));
                }
                RecordingAxis axis = RecordingAxis::Time;
                if (!parseRecordingAxis(order.at(index).toString(), axis))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must be 'Time', 'Z', or 'XY'")
                                    .arg(elementPath(memberPath(path, QStringLiteral("order")), index)));
                }
                parsed.order.push_back(axis);
            }

            QJsonArray positions;
            if (!readArray(object, QStringLiteral("positions"), positions, path, errorMessage))
            {
                return false;
            }
            parsed.positions.clear();
            parsed.positions.reserve(static_cast<size_t>(positions.size()));
            for (qsizetype index = 0; index < positions.size(); ++index)
            {
                if (!positions.at(index).isObject())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must be an object")
                                    .arg(elementPath(memberPath(path, QStringLiteral("positions")), index)));
                }
                QPointF position;
                if (!parsePoint(positions.at(index).toObject(),
                                position,
                                elementPath(memberPath(path, QStringLiteral("positions")), index),
                                errorMessage))
                {
                    return false;
                }
                parsed.positions.push_back(position);
            }

            QJsonArray zPositions;
            if (!readArray(object, QStringLiteral("zPositions"), zPositions, path, errorMessage))
            {
                return false;
            }
            parsed.zPositions.clear();
            parsed.zPositions.reserve(static_cast<size_t>(zPositions.size()));
            for (qsizetype index = 0; index < zPositions.size(); ++index)
            {
                const QJsonValue value = zPositions.at(index);
                if (!value.isDouble() || !isFinite(value.toDouble()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 must be a finite number")
                                    .arg(elementPath(memberPath(path, QStringLiteral("zPositions")), index)));
                }
                parsed.zPositions.push_back(value.toDouble());
            }

            if (!readString(object, QStringLiteral("saveDir"), parsed.saveDir, path, errorMessage)
                || !readString(object, QStringLiteral("baseName"), parsed.baseName, path, errorMessage)
                || !readString(object,
                               QStringLiteral("metadataFileName"),
                               parsed.metadataFileName,
                               path,
                               errorMessage)
                || !readString(object,
                               QStringLiteral("configPath"),
                               parsed.configPath,
                               path,
                               errorMessage)
                || !readString(object,
                               QStringLiteral("configSha256"),
                               parsed.configSha256,
                               path,
                               errorMessage))
            {
                return false;
            }

            QJsonObject processing;
            if (!readObject(object, QStringLiteral("processing"), processing, path, errorMessage)
                || !checkObjectFields(processing,
                                      {QStringLiteral("bitDepth"), QStringLiteral("modules")},
                                      memberPath(path, QStringLiteral("processing")),
                                      errorMessage))
            {
                return false;
            }
            int bitDepth = 0;
            if (!readInt(processing,
                         QStringLiteral("bitDepth"),
                         bitDepth,
                         memberPath(path, QStringLiteral("processing")),
                         errorMessage))
            {
                return false;
            }
            if (bitDepth == static_cast<int>(ProcessingBitDepth::Bit8))
            {
                parsed.processing.bitDepth = ProcessingBitDepth::Bit8;
            }
            else if (bitDepth == static_cast<int>(ProcessingBitDepth::Bit16))
            {
                parsed.processing.bitDepth = ProcessingBitDepth::Bit16;
            }
            else
            {
                return fail(errorMessage,
                            QStringLiteral("%1.processing.bitDepth must be 8 or 16").arg(path));
            }

            QJsonArray modules;
            if (!readArray(processing,
                           QStringLiteral("modules"),
                           modules,
                           memberPath(path, QStringLiteral("processing")),
                           errorMessage))
            {
                return false;
            }
            parsed.processing.modules.clear();
            parsed.processing.modules.reserve(modules.size());
            for (qsizetype index = 0; index < modules.size(); ++index)
            {
                const QString modulePath = elementPath(
                    memberPath(memberPath(path, QStringLiteral("processing")), QStringLiteral("modules")),
                    index);
                if (!modules.at(index).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(modulePath));
                }
                const QJsonObject moduleObject = modules.at(index).toObject();
                if (!checkObjectFields(moduleObject,
                                       {QStringLiteral("kind"),
                                        QStringLiteral("schemaVersion"),
                                        QStringLiteral("parameters")},
                                       modulePath,
                                       errorMessage))
                {
                    return false;
                }

                ProcessingModuleRecipe module;
                QString kindName;
                QJsonObject parameters;
                if (!readString(moduleObject,
                                QStringLiteral("kind"),
                                kindName,
                                modulePath,
                                errorMessage)
                    || !readInt(moduleObject,
                                QStringLiteral("schemaVersion"),
                                module.schemaVersion,
                                modulePath,
                                errorMessage)
                    || !readObject(moduleObject,
                                   QStringLiteral("parameters"),
                                   parameters,
                                   modulePath,
                                   errorMessage))
                {
                    return false;
                }
                module.moduleId = processingModuleIdFromDocument(kindName);
                if (module.moduleId.isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.kind has unknown processing module name '%2'")
                                    .arg(modulePath, kindName));
                }
                if (module.schemaVersion <= 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.schemaVersion must be positive; got %2")
                                    .arg(modulePath)
                                    .arg(module.schemaVersion));
                }
                module.parameters = processingParametersFromJson(module.moduleId, parameters);
                parsed.processing.modules.append(module);
            }

            QJsonObject userMetadata;
            if (!readObject(object,
                            QStringLiteral("userMetadata"),
                            userMetadata,
                            path,
                            errorMessage))
            {
                return false;
            }
            parsed.userMetadata = canonicalJsonObject(userMetadata);
            plan = parsed;
            return true;
        }

        bool parseAcquisitionEvent(const QJsonObject& object,
                                   AcquisitionEvent& event,
                                   const QString& path,
                                   QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("sequenceIndex"),
                                    QStringLiteral("burstIndex"),
                                    QStringLiteral("timeIndex"),
                                    QStringLiteral("zIndex"),
                                    QStringLiteral("positionIndex"),
                                    QStringLiteral("x"),
                                    QStringLiteral("y"),
                                    QStringLiteral("z"),
                                    QStringLiteral("hasXY"),
                                    QStringLiteral("hasZ"),
                                    QStringLiteral("exposureMs"),
                                    QStringLiteral("minimumStartTimeMs"),
                                    QStringLiteral("cameraIds")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            AcquisitionEvent parsed;
            if (!readUnsignedIntegerString(object,
                                           QStringLiteral("sequenceIndex"),
                                           parsed.sequenceIndex,
                                           path,
                                           errorMessage)
                || !readInt(object,
                            QStringLiteral("burstIndex"),
                            parsed.burstIndex,
                            path,
                            errorMessage)
                || !readInt(object,
                            QStringLiteral("timeIndex"),
                            parsed.timeIndex,
                            path,
                            errorMessage)
                || !readInt(object, QStringLiteral("zIndex"), parsed.zIndex, path, errorMessage)
                || !readInt(object,
                            QStringLiteral("positionIndex"),
                            parsed.positionIndex,
                            path,
                            errorMessage)
                || !readDouble(object, QStringLiteral("x"), parsed.x, path, errorMessage)
                || !readDouble(object, QStringLiteral("y"), parsed.y, path, errorMessage)
                || !readDouble(object, QStringLiteral("z"), parsed.z, path, errorMessage)
                || !readBool(object, QStringLiteral("hasXY"), parsed.hasXY, path, errorMessage)
                || !readBool(object, QStringLiteral("hasZ"), parsed.hasZ, path, errorMessage)
                || !readDouble(object,
                               QStringLiteral("exposureMs"),
                               parsed.exposureMs,
                               path,
                               errorMessage)
                || !readSignedIntegerString(object,
                                            QStringLiteral("minimumStartTimeMs"),
                                            parsed.minimumStartTimeMs,
                                            path,
                                            errorMessage))
            {
                return false;
            }

            QJsonArray cameraIds;
            if (!readArray(object, QStringLiteral("cameraIds"), cameraIds, path, errorMessage)
                || !parseStringArray(cameraIds,
                                     parsed.cameraIds,
                                     memberPath(path, QStringLiteral("cameraIds")),
                                     errorMessage))
            {
                return false;
            }
            event = parsed;
            return true;
        }

        bool parseFrameRecord(const QJsonObject& object,
                              FrameRecord& frame,
                              const QString& path,
                              QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("cameraId"),
                                    QStringLiteral("width"),
                                    QStringLiteral("height"),
                                    QStringLiteral("bitsPerSample"),
                                    QStringLiteral("pixelFormat"),
                                    QStringLiteral("frameIndex"),
                                    QStringLiteral("timestampNs"),
                                    QStringLiteral("sourceRoiX"),
                                    QStringLiteral("sourceRoiY"),
                                    QStringLiteral("sourceRoiWidth"),
                                    QStringLiteral("sourceRoiHeight")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            FrameRecord parsed;
            QString pixelFormat;
            if (!readString(object,
                            QStringLiteral("cameraId"),
                            parsed.cameraId,
                            path,
                            errorMessage)
                || !readInt(object, QStringLiteral("width"), parsed.width, path, errorMessage)
                || !readInt(object, QStringLiteral("height"), parsed.height, path, errorMessage)
                || !readInt(object,
                            QStringLiteral("bitsPerSample"),
                            parsed.bitsPerSample,
                            path,
                            errorMessage)
                || !readString(object,
                               QStringLiteral("pixelFormat"),
                               pixelFormat,
                               path,
                               errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("frameIndex"),
                                              parsed.frameIndex,
                                              path,
                                              errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("timestampNs"),
                                              parsed.timestampNs,
                                              path,
                                              errorMessage)
                || !readInt(object,
                            QStringLiteral("sourceRoiX"),
                            parsed.sourceRoiX,
                            path,
                            errorMessage)
                || !readInt(object,
                            QStringLiteral("sourceRoiY"),
                            parsed.sourceRoiY,
                            path,
                            errorMessage)
                || !readInt(object,
                            QStringLiteral("sourceRoiWidth"),
                            parsed.sourceRoiWidth,
                            path,
                            errorMessage)
                || !readInt(object,
                            QStringLiteral("sourceRoiHeight"),
                            parsed.sourceRoiHeight,
                            path,
                            errorMessage))
            {
                return false;
            }
            if (!parseImagePixelFormat(pixelFormat, parsed.pixelFormat))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.pixelFormat has unknown value '%2'").arg(path, pixelFormat));
            }
            frame = parsed;
            return true;
        }

        bool parseEventRecord(const QJsonObject& object,
                              AcquisitionEventRecord& record,
                              const QString& path,
                              QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("event"),
                                    QStringLiteral("startedTimestampNs"),
                                    QStringLiteral("completedTimestampNs"),
                                    QStringLiteral("succeeded"),
                                    QStringLiteral("errorMessage"),
                                    QStringLiteral("frames")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            AcquisitionEventRecord parsed;
            QJsonObject event;
            QJsonObject frames;
            if (!readObject(object, QStringLiteral("event"), event, path, errorMessage)
                || !parseAcquisitionEvent(event,
                                          parsed.event,
                                          memberPath(path, QStringLiteral("event")),
                                          errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("startedTimestampNs"),
                                              parsed.startedTimestampNs,
                                              path,
                                              errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("completedTimestampNs"),
                                              parsed.completedTimestampNs,
                                              path,
                                              errorMessage)
                || !readBool(object, QStringLiteral("succeeded"), parsed.succeeded, path, errorMessage)
                || !readString(object,
                               QStringLiteral("errorMessage"),
                               parsed.errorMessage,
                               path,
                               errorMessage)
                || !readObject(object, QStringLiteral("frames"), frames, path, errorMessage))
            {
                return false;
            }

            for (const QString& cameraId : frames.keys())
            {
                const QString framePath = QStringLiteral("%1.frames[%2]").arg(path, cameraId);
                if (!frames.value(cameraId).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(framePath));
                }
                FrameRecord frame;
                if (!parseFrameRecord(frames.value(cameraId).toObject(), frame, framePath, errorMessage))
                {
                    return false;
                }
                parsed.frames.insert(cameraId, frame);
            }
            record = parsed;
            return true;
        }

        bool parseSoftware(const QJsonObject& object,
                           SoftwareSnapshot& software,
                           const QString& path,
                           QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("applicationVersion"),
                                    QStringLiteral("coreVersion"),
                                    QStringLiteral("mmCoreVersion"),
                                    QStringLiteral("libTiffVersion"),
                                    QStringLiteral("zlibVersion"),
                                    QStringLiteral("operatingSystem")},
                                   path,
                                   errorMessage))
            {
                return false;
            }
            return readString(object,
                              QStringLiteral("applicationVersion"),
                              software.applicationVersion,
                              path,
                              errorMessage)
                && readString(object,
                              QStringLiteral("coreVersion"),
                              software.coreVersion,
                              path,
                              errorMessage)
                && readString(object,
                              QStringLiteral("mmCoreVersion"),
                              software.mmCoreVersion,
                              path,
                              errorMessage)
                && readString(object,
                              QStringLiteral("libTiffVersion"),
                              software.libTiffVersion,
                              path,
                              errorMessage)
                && readString(object,
                              QStringLiteral("zlibVersion"),
                              software.zlibVersion,
                              path,
                              errorMessage)
                && readString(object,
                              QStringLiteral("operatingSystem"),
                              software.operatingSystem,
                              path,
                              errorMessage);
        }

        bool parseOutput(const QJsonObject& object,
                         RecordingOutputManifest& output,
                         const QString& path,
                         QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("streamedToDisk"), QStringLiteral("files")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            RecordingOutputManifest parsed;
            QJsonObject files;
            if (!readBool(object,
                          QStringLiteral("streamedToDisk"),
                          parsed.streamedToDisk,
                          path,
                          errorMessage)
                || !readObject(object, QStringLiteral("files"), files, path, errorMessage))
            {
                return false;
            }
            for (const QString& cameraId : files.keys())
            {
                const QString filePath = QStringLiteral("%1.files[%2]").arg(path, cameraId);
                if (!files.value(cameraId).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(filePath));
                }
                const QJsonObject fileObject = files.value(cameraId).toObject();
                if (!checkObjectFields(fileObject,
                                       {QStringLiteral("rawPath"),
                                        QStringLiteral("frameInfoPath"),
                                        QStringLiteral("framesWritten")},
                                       filePath,
                                       errorMessage))
                {
                    return false;
                }
                RecordingFileManifest file;
                if (!readString(fileObject,
                                QStringLiteral("rawPath"),
                                file.rawPath,
                                filePath,
                                errorMessage)
                    || !readString(fileObject,
                                   QStringLiteral("frameInfoPath"),
                                   file.frameInfoPath,
                                   filePath,
                                   errorMessage)
                    || !readSignedIntegerString(fileObject,
                                                QStringLiteral("framesWritten"),
                                                file.framesWritten,
                                                filePath,
                                                errorMessage))
                {
                    return false;
                }
                parsed.files.insert(cameraId, file);
            }
            output = parsed;
            return true;
        }

        bool parseLayer(const QJsonObject& object,
                        DocumentLayer& layer,
                        const QString& path,
                        QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("id"),
                                    QStringLiteral("sourceId"),
                                    QStringLiteral("name"),
                                    QStringLiteral("kind"),
                                    QStringLiteral("width"),
                                    QStringLiteral("height"),
                                    QStringLiteral("pixelToSensor"),
                                    QStringLiteral("display")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            DocumentLayer parsed;
            QString kind;
            QJsonObject transform;
            QJsonObject display;
            if (!readString(object, QStringLiteral("id"), parsed.id, path, errorMessage)
                || !readString(object,
                               QStringLiteral("sourceId"),
                               parsed.sourceId,
                               path,
                               errorMessage)
                || !readString(object, QStringLiteral("name"), parsed.name, path, errorMessage)
                || !readString(object, QStringLiteral("kind"), kind, path, errorMessage)
                || !readInt(object, QStringLiteral("width"), parsed.width, path, errorMessage)
                || !readInt(object, QStringLiteral("height"), parsed.height, path, errorMessage)
                || !readObject(object,
                               QStringLiteral("pixelToSensor"),
                               transform,
                               path,
                               errorMessage)
                || !parseTransform(transform,
                                   parsed.pixelToSensor,
                                   memberPath(path, QStringLiteral("pixelToSensor")),
                                   errorMessage)
                || !readObject(object, QStringLiteral("display"), display, path, errorMessage)
                || !parseDisplayState(display,
                                      parsed.display,
                                      memberPath(path, QStringLiteral("display")),
                                      errorMessage))
            {
                return false;
            }
            if (!parseDocumentLayerKind(kind, parsed.kind))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.kind has unknown layer kind '%2'").arg(path, kind));
            }
            layer = parsed;
            return true;
        }

        bool parseMarkup(const QJsonObject& object,
                         DocumentMarkup& markup,
                         const QString& path,
                         QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("id"),
                                    QStringLiteral("type"),
                                    QStringLiteral("role"),
                                    QStringLiteral("layerId"),
                                    QStringLiteral("label"),
                                    QStringLiteral("start"),
                                    QStringLiteral("end"),
                                    QStringLiteral("rect"),
                                    QStringLiteral("visible"),
                                    QStringLiteral("selected")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            DocumentMarkup parsed;
            QString type;
            QString role;
            QJsonObject start;
            QJsonObject end;
            QJsonObject rect;
            if (!readString(object, QStringLiteral("id"), parsed.id, path, errorMessage)
                || !readString(object, QStringLiteral("type"), type, path, errorMessage)
                || !readString(object, QStringLiteral("role"), role, path, errorMessage)
                || !readString(object,
                               QStringLiteral("layerId"),
                               parsed.layerId,
                               path,
                               errorMessage)
                || !readString(object, QStringLiteral("label"), parsed.label, path, errorMessage)
                || !readObject(object, QStringLiteral("start"), start, path, errorMessage)
                || !parsePoint(start,
                               parsed.start,
                               memberPath(path, QStringLiteral("start")),
                               errorMessage)
                || !readObject(object, QStringLiteral("end"), end, path, errorMessage)
                || !parsePoint(end,
                               parsed.end,
                               memberPath(path, QStringLiteral("end")),
                               errorMessage)
                || !readObject(object, QStringLiteral("rect"), rect, path, errorMessage)
                || !parseRect(rect,
                              parsed.rect,
                              memberPath(path, QStringLiteral("rect")),
                              errorMessage)
                || !readBool(object, QStringLiteral("visible"), parsed.visible, path, errorMessage)
                || !readBool(object, QStringLiteral("selected"), parsed.selected, path, errorMessage))
            {
                return false;
            }
            if (!parseDocumentMarkupType(type, parsed.type))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.type has unknown markup type '%2'").arg(path, type));
            }
            if (!parseDocumentMarkupRole(role, parsed.role))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.role has unknown markup role '%2'").arg(path, role));
            }
            markup = parsed;
            return true;
        }

        bool parseExperimentDocumentObject(const QJsonObject& object,
                                           ExperimentDocument& document,
                                           const QString& path,
                                           QString* errorMessage)
        {
            if (!checkObjectFields(object,
                                   {QStringLiteral("schemaVersion"),
                                    QStringLiteral("plan"),
                                    QStringLiteral("runState"),
                                    QStringLiteral("startedTimestampNs"),
                                    QStringLiteral("completedTimestampNs"),
                                    QStringLiteral("errorMessage"),
                                    QStringLiteral("software"),
                                    QStringLiteral("deviceProperties"),
                                    QStringLiteral("events"),
                                    QStringLiteral("output"),
                                    QStringLiteral("layers"),
                                    QStringLiteral("markups")},
                                   path,
                                   errorMessage))
            {
                return false;
            }

            ExperimentDocument parsed;
            if (!readInt(object,
                         QStringLiteral("schemaVersion"),
                         parsed.schemaVersion,
                         path,
                         errorMessage))
            {
                return false;
            }
            if (parsed.schemaVersion != kExperimentDocumentSchemaVersion)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.schemaVersion %2 is unsupported; expected %3")
                                .arg(path)
                                .arg(parsed.schemaVersion)
                                .arg(kExperimentDocumentSchemaVersion));
            }

            QJsonObject plan;
            QString runState;
            QJsonObject software;
            QJsonObject deviceProperties;
            if (!readObject(object, QStringLiteral("plan"), plan, path, errorMessage)
                || !parseExperimentPlanObject(plan,
                                              parsed.plan,
                                              memberPath(path, QStringLiteral("plan")),
                                              errorMessage)
                || !readString(object,
                               QStringLiteral("runState"),
                               runState,
                               path,
                               errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("startedTimestampNs"),
                                              parsed.startedTimestampNs,
                                              path,
                                              errorMessage)
                || !readUnsignedIntegerString(object,
                                              QStringLiteral("completedTimestampNs"),
                                              parsed.completedTimestampNs,
                                              path,
                                              errorMessage)
                || !readString(object,
                               QStringLiteral("errorMessage"),
                               parsed.errorMessage,
                               path,
                               errorMessage)
                || !readObject(object, QStringLiteral("software"), software, path, errorMessage)
                || !parseSoftware(software,
                                  parsed.software,
                                  memberPath(path, QStringLiteral("software")),
                                  errorMessage)
                || !readObject(object,
                               QStringLiteral("deviceProperties"),
                               deviceProperties,
                               path,
                               errorMessage))
            {
                return false;
            }
            if (!parseExperimentRunState(runState, parsed.runState))
            {
                return fail(errorMessage,
                            QStringLiteral("%1.runState has unknown state '%2'").arg(path, runState));
            }
            parsed.deviceProperties = canonicalJsonObject(deviceProperties);

            QJsonArray events;
            if (!readArray(object, QStringLiteral("events"), events, path, errorMessage))
            {
                return false;
            }
            parsed.events.clear();
            parsed.events.reserve(events.size());
            for (qsizetype index = 0; index < events.size(); ++index)
            {
                const QString eventPath = elementPath(memberPath(path, QStringLiteral("events")), index);
                if (!events.at(index).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(eventPath));
                }
                AcquisitionEventRecord record;
                if (!parseEventRecord(events.at(index).toObject(), record, eventPath, errorMessage))
                {
                    return false;
                }
                parsed.events.append(record);
            }

            QJsonObject output;
            if (!readObject(object, QStringLiteral("output"), output, path, errorMessage)
                || !parseOutput(output,
                                parsed.output,
                                memberPath(path, QStringLiteral("output")),
                                errorMessage))
            {
                return false;
            }

            QJsonArray layers;
            if (!readArray(object, QStringLiteral("layers"), layers, path, errorMessage))
            {
                return false;
            }
            parsed.layers.clear();
            parsed.layers.reserve(layers.size());
            for (qsizetype index = 0; index < layers.size(); ++index)
            {
                const QString layerPath = elementPath(memberPath(path, QStringLiteral("layers")), index);
                if (!layers.at(index).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(layerPath));
                }
                DocumentLayer layer;
                if (!parseLayer(layers.at(index).toObject(), layer, layerPath, errorMessage))
                {
                    return false;
                }
                parsed.layers.append(layer);
            }

            QJsonArray markups;
            if (!readArray(object, QStringLiteral("markups"), markups, path, errorMessage))
            {
                return false;
            }
            parsed.markups.clear();
            parsed.markups.reserve(markups.size());
            for (qsizetype index = 0; index < markups.size(); ++index)
            {
                const QString markupPath = elementPath(memberPath(path, QStringLiteral("markups")), index);
                if (!markups.at(index).isObject())
                {
                    return fail(errorMessage, QStringLiteral("%1 must be an object").arg(markupPath));
                }
                DocumentMarkup markup;
                if (!parseMarkup(markups.at(index).toObject(), markup, markupPath, errorMessage))
                {
                    return false;
                }
                parsed.markups.append(markup);
            }
            document = parsed;
            return true;
        }

        bool validateExperimentDocumentImpl(const ExperimentDocument& document,
                                            const QString& path,
                                            QString* errorMessage)
        {
            if (document.schemaVersion != kExperimentDocumentSchemaVersion)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.schemaVersion %2 is unsupported; expected %3")
                                .arg(path)
                                .arg(document.schemaVersion)
                                .arg(kExperimentDocumentSchemaVersion));
            }
            if (!validateExperimentPlanImpl(document.plan,
                                            memberPath(path, QStringLiteral("plan")),
                                            errorMessage))
            {
                return false;
            }

            if (document.completedTimestampNs != 0 && document.startedTimestampNs == 0)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.completedTimestampNs requires a non-zero startedTimestampNs")
                                .arg(path));
            }
            if (document.completedTimestampNs != 0
                && document.completedTimestampNs < document.startedTimestampNs)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.completedTimestampNs must not precede startedTimestampNs")
                                .arg(path));
            }
            switch (document.runState)
            {
            case ExperimentRunState::Draft:
                if (document.startedTimestampNs != 0 || document.completedTimestampNs != 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Draft state requires zero run timestamps").arg(path));
                }
                if (!document.errorMessage.isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.errorMessage must be empty in Draft state").arg(path));
                }
                break;
            case ExperimentRunState::Running:
                if (document.startedTimestampNs == 0 || document.completedTimestampNs != 0)
                {
                    return fail(errorMessage,
                                QStringLiteral(
                                    "%1 Running state requires a start timestamp and no completion timestamp")
                                    .arg(path));
                }
                if (!document.errorMessage.isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.errorMessage must be empty in Running state").arg(path));
                }
                break;
            case ExperimentRunState::Completed:
                if (document.startedTimestampNs == 0 || document.completedTimestampNs == 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Completed state requires start and completion timestamps")
                                    .arg(path));
                }
                if (!document.errorMessage.isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.errorMessage must be empty in Completed state").arg(path));
                }
                break;
            case ExperimentRunState::Canceled:
                if (document.startedTimestampNs == 0 || document.completedTimestampNs == 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Canceled state requires start and completion timestamps")
                                    .arg(path));
                }
                break;
            case ExperimentRunState::Failed:
                if (document.startedTimestampNs == 0 || document.completedTimestampNs == 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Failed state requires start and completion timestamps")
                                    .arg(path));
                }
                if (document.errorMessage.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.errorMessage must describe the failure").arg(path));
                }
                break;
            default:
                return fail(errorMessage, QStringLiteral("%1.runState is invalid").arg(path));
            }

            if (!validateJsonValue(document.deviceProperties,
                                   memberPath(path, QStringLiteral("deviceProperties")),
                                   errorMessage))
            {
                return false;
            }

            QSet<quint64> sequenceIndices;
            quint64 previousSequenceIndex = 0;
            bool hasPreviousSequenceIndex = false;
            for (qsizetype index = 0; index < document.events.size(); ++index)
            {
                const QString eventPath = elementPath(memberPath(path, QStringLiteral("events")), index);
                const quint64 sequenceIndex = document.events.at(index).event.sequenceIndex;
                if (sequenceIndices.contains(sequenceIndex))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.event.sequenceIndex duplicates value %2")
                                    .arg(eventPath)
                                    .arg(sequenceIndex));
                }
                if (hasPreviousSequenceIndex && sequenceIndex < previousSequenceIndex)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.event.sequenceIndex must be in ascending order")
                                    .arg(eventPath));
                }
                sequenceIndices.insert(sequenceIndex);
                previousSequenceIndex = sequenceIndex;
                hasPreviousSequenceIndex = true;
                if (!validateAcquisitionEventRecord(document.events.at(index),
                                                    document.plan,
                                                    eventPath,
                                                    errorMessage))
                {
                    return false;
                }
            }

            if (document.output.streamedToDisk && !document.plan.streamToDisk)
            {
                return fail(errorMessage,
                            QStringLiteral("%1.output.streamedToDisk cannot be true when plan.streamToDisk is false")
                                .arg(path));
            }
            const QSet<QString> planCameraIds(document.plan.cameraIds.constBegin(),
                                              document.plan.cameraIds.constEnd());
            for (auto it = document.output.files.constBegin(); it != document.output.files.constEnd(); ++it)
            {
                const QString filePath = QStringLiteral("%1.output.files[%2]").arg(path, it.key());
                if (!validateIdentifier(it.key(), filePath + QStringLiteral(" key"), errorMessage))
                {
                    return false;
                }
                if (!planCameraIds.contains(it.key()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 uses camera ID '%2' that is not in the plan")
                                    .arg(filePath, it.key()));
                }
                const RecordingFileManifest& file = it.value();
                if (file.framesWritten < 0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.framesWritten must be non-negative").arg(filePath));
                }
                if (file.framesWritten > 0 && file.rawPath.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.rawPath must not be empty when frames were written")
                                    .arg(filePath));
                }
                if (document.output.streamedToDisk
                    && document.plan.format == RecordingFormat::Binary
                    && file.framesWritten > 0
                    && file.frameInfoPath.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.frameInfoPath is required for Binary output")
                                    .arg(filePath));
                }
            }
            if (document.runState == ExperimentRunState::Completed && document.plan.streamToDisk)
            {
                if (!document.output.streamedToDisk)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Completed streamed run requires streamed output").arg(path));
                }
                for (const QString& cameraId : document.plan.cameraIds)
                {
                    const auto file = document.output.files.constFind(cameraId);
                    if (file == document.output.files.constEnd() || file->framesWritten < 1)
                    {
                        return fail(errorMessage,
                                    QStringLiteral("%1 Completed streamed run has no frames for camera '%2'")
                                        .arg(path, cameraId));
                    }
                }
                bool hasSucceededFrame = false;
                for (const AcquisitionEventRecord& event : document.events)
                {
                    if (event.succeeded && !event.frames.isEmpty())
                    {
                        hasSucceededFrame = true;
                        break;
                    }
                }
                if (!hasSucceededFrame)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 Completed streamed run requires a successful frame event")
                                    .arg(path));
                }
            }

            QSet<QString> layerIds;
            for (qsizetype index = 0; index < document.layers.size(); ++index)
            {
                const DocumentLayer& layer = document.layers.at(index);
                const QString layerPath = elementPath(memberPath(path, QStringLiteral("layers")), index);
                if (!validateIdentifier(layer.id,
                                        memberPath(layerPath, QStringLiteral("id")),
                                        errorMessage)
                    || !validateIdentifier(layer.sourceId,
                                           memberPath(layerPath, QStringLiteral("sourceId")),
                                           errorMessage))
                {
                    return false;
                }
                if (layerIds.contains(layer.id))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.id duplicates layer ID '%2'").arg(layerPath, layer.id));
                }
                layerIds.insert(layer.id);
                if (layer.name.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.name must not be empty").arg(layerPath));
                }
                switch (layer.kind)
                {
                case DocumentLayerKind::Raw:
                case DocumentLayerKind::Processed:
                case DocumentLayerKind::Static:
                case DocumentLayerKind::Gallery:
                    break;
                default:
                    return fail(errorMessage, QStringLiteral("%1.kind is invalid").arg(layerPath));
                }
                if (layer.width < 1 || layer.height < 1)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 width and height must both be positive").arg(layerPath));
                }
                if (!validateTransform(layer.pixelToSensor,
                                       memberPath(layerPath, QStringLiteral("pixelToSensor")),
                                       errorMessage))
                {
                    return false;
                }
                const LayerDisplayState& display = layer.display;
                const QString displayPath = memberPath(layerPath, QStringLiteral("display"));
                if (display.opacityPercent < 0 || display.opacityPercent > 100)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.opacityPercent must be between 0 and 100")
                                    .arg(displayPath));
                }
                if (!isFinite(display.gamma) || display.gamma <= 0.0)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.gamma must be a finite positive number").arg(displayPath));
                }
                if (display.colormap.trimmed().isEmpty() || display.blending.trimmed().isEmpty())
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 colormap and blending names must not be empty")
                                    .arg(displayPath));
                }
                if (display.levelDomainMax < 1
                    || display.levelMin < 0
                    || display.levelMin > display.levelMax
                    || display.levelMax > display.levelDomainMax)
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 levels must satisfy 0 <= levelMin <= levelMax <= levelDomainMax")
                                    .arg(displayPath));
                }
            }

            QSet<QString> markupIds;
            for (qsizetype index = 0; index < document.markups.size(); ++index)
            {
                const DocumentMarkup& markup = document.markups.at(index);
                const QString markupPath = elementPath(memberPath(path, QStringLiteral("markups")), index);
                if (!validateIdentifier(markup.id,
                                        memberPath(markupPath, QStringLiteral("id")),
                                        errorMessage)
                    || !validateIdentifier(markup.layerId,
                                           memberPath(markupPath, QStringLiteral("layerId")),
                                           errorMessage))
                {
                    return false;
                }
                if (markupIds.contains(markup.id))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.id duplicates markup ID '%2'")
                                    .arg(markupPath, markup.id));
                }
                markupIds.insert(markup.id);
                if (!layerIds.contains(markup.layerId))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.layerId references missing layer '%2'")
                                    .arg(markupPath, markup.layerId));
                }
                if (markup.type != DocumentMarkupType::Line && markup.type != DocumentMarkupType::Rect)
                {
                    return fail(errorMessage, QStringLiteral("%1.type is invalid").arg(markupPath));
                }
                switch (markup.role)
                {
                case DocumentMarkupRole::Generic:
                case DocumentMarkupRole::CrossSection:
                case DocumentMarkupRole::Roi:
                case DocumentMarkupRole::Measurement:
                    break;
                default:
                    return fail(errorMessage, QStringLiteral("%1.role is invalid").arg(markupPath));
                }
                if (!isFinite(markup.start.x()) || !isFinite(markup.start.y())
                    || !isFinite(markup.end.x()) || !isFinite(markup.end.y())
                    || !isFinite(markup.rect.x()) || !isFinite(markup.rect.y())
                    || !isFinite(markup.rect.width()) || !isFinite(markup.rect.height()))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1 geometry must contain only finite values").arg(markupPath));
                }
                if (markup.type == DocumentMarkupType::Rect
                    && (markup.rect.width() <= 0.0 || markup.rect.height() <= 0.0))
                {
                    return fail(errorMessage,
                                QStringLiteral("%1.rect width and height must be positive").arg(markupPath));
                }
            }
            return true;
        }

        QJsonObject experimentDocumentToJsonImpl(const ExperimentDocument& document)
        {
            QJsonArray events;
            for (const AcquisitionEventRecord& record : document.events)
            {
                events.append(eventRecordToJson(record));
            }
            QJsonArray layers;
            for (const DocumentLayer& layer : document.layers)
            {
                layers.append(layerToJson(layer));
            }
            QJsonArray markups;
            for (const DocumentMarkup& markup : document.markups)
            {
                markups.append(markupToJson(markup));
            }

            QJsonObject object;
            object.insert(QStringLiteral("schemaVersion"), document.schemaVersion);
            object.insert(QStringLiteral("plan"), experimentPlanToJsonImpl(document.plan));
            object.insert(QStringLiteral("runState"), experimentRunStateName(document.runState));
            object.insert(QStringLiteral("startedTimestampNs"), QString::number(document.startedTimestampNs));
            object.insert(QStringLiteral("completedTimestampNs"), QString::number(document.completedTimestampNs));
            object.insert(QStringLiteral("errorMessage"), document.errorMessage);
            object.insert(QStringLiteral("software"), softwareToJson(document.software));
            object.insert(QStringLiteral("deviceProperties"), canonicalJsonObject(document.deviceProperties));
            object.insert(QStringLiteral("events"), events);
            object.insert(QStringLiteral("output"), outputToJson(document.output));
            object.insert(QStringLiteral("layers"), layers);
            object.insert(QStringLiteral("markups"), markups);
            return canonicalJsonObject(object);
        }
    }

    QString recordingAxisName(RecordingAxis axis)
    {
        switch (axis)
        {
        case RecordingAxis::Time:
            return QStringLiteral("Time");
        case RecordingAxis::Z:
            return QStringLiteral("Z");
        case RecordingAxis::XY:
            return QStringLiteral("XY");
        }
        return QStringLiteral("Unknown");
    }

    QString experimentRunStateName(ExperimentRunState state)
    {
        switch (state)
        {
        case ExperimentRunState::Draft:
            return QStringLiteral("Draft");
        case ExperimentRunState::Running:
            return QStringLiteral("Running");
        case ExperimentRunState::Completed:
            return QStringLiteral("Completed");
        case ExperimentRunState::Canceled:
            return QStringLiteral("Canceled");
        case ExperimentRunState::Failed:
            return QStringLiteral("Failed");
        }
        return QStringLiteral("Unknown");
    }

    QString documentLayerKindName(DocumentLayerKind kind)
    {
        switch (kind)
        {
        case DocumentLayerKind::Raw:
            return QStringLiteral("Raw");
        case DocumentLayerKind::Processed:
            return QStringLiteral("Processed");
        case DocumentLayerKind::Static:
            return QStringLiteral("Static");
        case DocumentLayerKind::Tool:
            return QStringLiteral("Tool");
        case DocumentLayerKind::Gallery:
            return QStringLiteral("Gallery");
        }
        return QStringLiteral("Unknown");
    }

    QString documentMarkupTypeName(DocumentMarkupType type)
    {
        switch (type)
        {
        case DocumentMarkupType::Line:
            return QStringLiteral("Line");
        case DocumentMarkupType::Rect:
            return QStringLiteral("Rect");
        }
        return QStringLiteral("Unknown");
    }

    QString documentMarkupRoleName(DocumentMarkupRole role)
    {
        switch (role)
        {
        case DocumentMarkupRole::Generic:
            return QStringLiteral("Generic");
        case DocumentMarkupRole::CrossSection:
            return QStringLiteral("CrossSection");
        case DocumentMarkupRole::Roi:
            return QStringLiteral("Roi");
        case DocumentMarkupRole::Measurement:
            return QStringLiteral("Measurement");
        }
        return QStringLiteral("Unknown");
    }

    QPointF ImageTransform2D::map(const QPointF& point) const
    {
        return QPointF(m11 * point.x() + m21 * point.y() + dx,
                       m12 * point.x() + m22 * point.y() + dy);
    }

    bool ImageTransform2D::inverted(ImageTransform2D& inverse) const
    {
        if (!isFinite(m11) || !isFinite(m12) || !isFinite(m21)
            || !isFinite(m22) || !isFinite(dx) || !isFinite(dy))
        {
            return false;
        }
        const double determinant = m11 * m22 - m12 * m21;
        if (!isFinite(determinant) || determinant == 0.0)
        {
            return false;
        }

        ImageTransform2D result;
        result.m11 = m22 / determinant;
        result.m12 = -m12 / determinant;
        result.m21 = -m21 / determinant;
        result.m22 = m11 / determinant;
        result.dx = -(result.m11 * dx + result.m21 * dy);
        result.dy = -(result.m12 * dx + result.m22 * dy);
        if (!isFinite(result.m11) || !isFinite(result.m12) || !isFinite(result.m21)
            || !isFinite(result.m22) || !isFinite(result.dx) || !isFinite(result.dy))
        {
            return false;
        }
        inverse = result;
        return true;
    }

    bool validateExperimentPlan(const ExperimentPlan& plan, QString* errorMessage)
    {
        clearError(errorMessage);
        return validateExperimentPlanImpl(plan, QStringLiteral("plan"), errorMessage);
    }

    bool validateExperimentDocument(const ExperimentDocument& document, QString* errorMessage)
    {
        clearError(errorMessage);
        return validateExperimentDocumentImpl(document, QStringLiteral("document"), errorMessage);
    }

    QList<AcquisitionEvent> buildAcquisitionEvents(const ExperimentPlan& plan,
                                                   int burstIndex,
                                                   QString* errorMessage)
    {
        clearError(errorMessage);
        if (!validateExperimentPlanImpl(plan, QStringLiteral("plan"), errorMessage))
        {
            return {};
        }
        const int burstCount = plan.burstMode ? plan.targetBursts : 1;
        if (burstIndex < 0 || burstIndex >= burstCount)
        {
            fail(errorMessage,
                 QStringLiteral("burstIndex %1 is outside the valid range 0 to %2")
                     .arg(burstIndex)
                     .arg(burstCount - 1));
            return {};
        }

        quint64 eventsPerBurst = 0;
        if (!perBurstEventCount(plan, eventsPerBurst, errorMessage, QStringLiteral("plan")))
        {
            return {};
        }
        QList<AcquisitionEvent> events;
        events.reserve(static_cast<qsizetype>(eventsPerBurst));

        const int timeCount = plan.framesPerBurst;
        const int zCount = plan.zPositions.empty() ? 1 : static_cast<int>(plan.zPositions.size());
        const int positionCount = plan.positions.empty() ? 1 : static_cast<int>(plan.positions.size());
        int timeIndex = 0;
        int zIndex = 0;
        int positionIndex = 0;

        const auto axisCount = [&](RecordingAxis axis)
        {
            switch (axis)
            {
            case RecordingAxis::Time:
                return timeCount;
            case RecordingAxis::Z:
                return zCount;
            case RecordingAxis::XY:
                return positionCount;
            }
            return 0;
        };

        std::function<void(size_t)> appendEvents = [&](size_t depth)
        {
            if (depth == plan.order.size())
            {
                AcquisitionEvent event;
                event.sequenceIndex = static_cast<quint64>(burstIndex) * eventsPerBurst
                    + static_cast<quint64>(events.size());
                event.burstIndex = burstIndex;
                event.timeIndex = timeIndex;
                event.zIndex = zIndex;
                event.positionIndex = positionIndex;
                event.hasXY = !plan.positions.empty();
                event.hasZ = !plan.zPositions.empty();
                if (event.hasXY)
                {
                    const QPointF& position = plan.positions.at(static_cast<size_t>(positionIndex));
                    event.x = position.x();
                    event.y = position.y();
                }
                if (event.hasZ)
                {
                    event.z = plan.zPositions.at(static_cast<size_t>(zIndex));
                }
                event.exposureMs = plan.exposureMs;
                const long double minimumStart = static_cast<long double>(burstIndex)
                        * static_cast<long double>(plan.burstIntervalMs)
                    + static_cast<long double>(timeIndex)
                        * static_cast<long double>(plan.mdaIntervalMs);
                event.minimumStartTimeMs = static_cast<qint64>(minimumStart);
                event.cameraIds = plan.cameraIds;
                events.append(event);
                return;
            }

            const RecordingAxis axis = plan.order.at(depth);
            const int count = axisCount(axis);
            for (int index = 0; index < count; ++index)
            {
                switch (axis)
                {
                case RecordingAxis::Time:
                    timeIndex = index;
                    break;
                case RecordingAxis::Z:
                    zIndex = index;
                    break;
                case RecordingAxis::XY:
                    positionIndex = index;
                    break;
                }
                appendEvents(depth + 1);
            }
        };
        appendEvents(0);
        return events;
    }

    FrameRecord frameRecordFromImageFrame(const ImageFrame& frame)
    {
        FrameRecord record;
        record.cameraId = frame.cameraId;
        record.width = frame.width;
        record.height = frame.height;
        record.bitsPerSample = frame.bitsPerSample;
        record.pixelFormat = frame.pixelFormat;
        record.frameIndex = frame.frameIndex;
        record.timestampNs = frame.timestampNs;
        record.sourceRoiX = frame.sourceRoiX;
        record.sourceRoiY = frame.sourceRoiY;
        record.sourceRoiWidth = frame.sourceRoiWidth;
        record.sourceRoiHeight = frame.sourceRoiHeight;
        return record;
    }

    ImageTransform2D imagePixelToSensorTransform(const ImageFrame& frame)
    {
        ImageTransform2D transform;
        if (frame.width <= 0 || frame.height <= 0 || !frame.hasSourceRoi())
        {
            return transform;
        }

        const double scaleX = static_cast<double>(frame.sourceRoiWidth) / static_cast<double>(frame.width);
        const double scaleY = static_cast<double>(frame.sourceRoiHeight) / static_cast<double>(frame.height);
        transform.m11 = scaleX;
        transform.m22 = scaleY;
        transform.dx = static_cast<double>(frame.sourceRoiX) + (scaleX - 1.0) * 0.5;
        transform.dy = static_cast<double>(frame.sourceRoiY) + (scaleY - 1.0) * 0.5;
        return transform;
    }

    QJsonObject experimentPlanToJson(const ExperimentPlan& plan)
    {
        return experimentPlanToJsonImpl(plan);
    }

    bool experimentPlanFromJson(const QJsonObject& object,
                                ExperimentPlan& plan,
                                QString* errorMessage)
    {
        clearError(errorMessage);
        ExperimentPlan parsed;
        if (!parseExperimentPlanObject(object, parsed, QStringLiteral("plan"), errorMessage)
            || !validateExperimentPlanImpl(parsed, QStringLiteral("plan"), errorMessage))
        {
            return false;
        }
        plan = parsed;
        return true;
    }

    QJsonObject experimentDocumentToJson(const ExperimentDocument& document)
    {
        return experimentDocumentToJsonImpl(document);
    }

    bool experimentDocumentFromJson(const QJsonObject& object,
                                    ExperimentDocument& document,
                                    QString* errorMessage)
    {
        clearError(errorMessage);
        ExperimentDocument parsed;
        if (!parseExperimentDocumentObject(object, parsed, QStringLiteral("document"), errorMessage)
            || !validateExperimentDocumentImpl(parsed, QStringLiteral("document"), errorMessage))
        {
            return false;
        }
        document = parsed;
        return true;
    }

    QByteArray serializeExperimentDocument(const ExperimentDocument& document)
    {
        return QJsonDocument(experimentDocumentToJsonImpl(document)).toJson(QJsonDocument::Indented);
    }

    bool deserializeExperimentDocument(const QByteArray& json,
                                       ExperimentDocument& document,
                                       QString* errorMessage)
    {
        clearError(errorMessage);
        if (json.trimmed().isEmpty())
        {
            return fail(errorMessage, QStringLiteral("Experiment document JSON is empty"));
        }

        QJsonParseError parseError{};
        const QJsonDocument jsonDocument = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            return fail(errorMessage,
                        QStringLiteral("Invalid experiment document JSON at byte %1: %2")
                            .arg(parseError.offset)
                            .arg(parseError.errorString()));
        }
        if (!jsonDocument.isObject())
        {
            return fail(errorMessage, QStringLiteral("Experiment document JSON root must be an object"));
        }
        return experimentDocumentFromJson(jsonDocument.object(), document, errorMessage);
    }

    bool saveExperimentDocument(const QString& filePath,
                                const ExperimentDocument& document,
                                QString* errorMessage)
    {
        clearError(errorMessage);
        if (filePath.trimmed().isEmpty())
        {
            return fail(errorMessage, QStringLiteral("Experiment document file path is empty"));
        }
        if (!validateExperimentDocumentImpl(document, QStringLiteral("document"), errorMessage))
        {
            return false;
        }

        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly))
        {
            return fail(errorMessage,
                        QStringLiteral("Cannot open experiment document '%1' for writing: %2")
                            .arg(filePath, file.errorString()));
        }
        const QByteArray json = serializeExperimentDocument(document);
        const qint64 bytesWritten = file.write(json);
        if (bytesWritten != json.size())
        {
            return fail(errorMessage,
                        QStringLiteral("Cannot write experiment document '%1': %2")
                            .arg(filePath, file.errorString()));
        }
        if (!file.commit())
        {
            return fail(errorMessage,
                        QStringLiteral("Cannot commit experiment document '%1': %2")
                            .arg(filePath, file.errorString()));
        }
        return true;
    }

    bool loadExperimentDocument(const QString& filePath,
                                ExperimentDocument& document,
                                QString* errorMessage)
    {
        clearError(errorMessage);
        if (filePath.trimmed().isEmpty())
        {
            return fail(errorMessage, QStringLiteral("Experiment document file path is empty"));
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            return fail(errorMessage,
                        QStringLiteral("Cannot open experiment document '%1' for reading: %2")
                            .arg(filePath, file.errorString()));
        }
        const QByteArray json = file.readAll();
        if (file.error() != QFileDevice::NoError)
        {
            return fail(errorMessage,
                        QStringLiteral("Cannot read experiment document '%1': %2")
                            .arg(filePath, file.errorString()));
        }

        ExperimentDocument parsed;
        if (!deserializeExperimentDocument(json, parsed, errorMessage))
        {
            if (errorMessage && !errorMessage->isEmpty())
            {
                *errorMessage = QStringLiteral("Cannot load experiment document '%1': %2")
                                    .arg(filePath, *errorMessage);
            }
            return false;
        }
        document = parsed;
        return true;
    }
}
