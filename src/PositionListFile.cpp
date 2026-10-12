#include "PositionListFile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace scopeone::gui
{
    namespace
    {
        QJsonObject scalar(const QString& type, const QJsonValue& value)
        {
            return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("scalar"), value}};
        }

        QJsonObject list(const QString& type, const QJsonArray& values)
        {
            return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("array"), values}};
        }

        // The first value of a typed property of a Micro-Manager property map
        QJsonValue firstValue(const QJsonObject& map, const QString& key)
        {
            const QJsonObject property = map.value(key).toObject();
            if (property.contains(QStringLiteral("scalar")))
            {
                return property.value(QStringLiteral("scalar"));
            }
            return property.value(QStringLiteral("array")).toArray().first();
        }

        bool fail(QString* errorMessage, const QString& message)
        {
            if (errorMessage) *errorMessage = message;
            return false;
        }
    }

    bool PositionListFile::save(const QString& path,
                                const QList<ListedPosition>& positions,
                                const QString& xyStage,
                                const QString& focusStage,
                                QString* errorMessage)
    {
        QJsonArray stagePositions;
        for (const ListedPosition& position : positions)
        {
            QJsonArray devices;
            if (!xyStage.isEmpty())
            {
                devices.append(QJsonObject{
                    {QStringLiteral("Device"), scalar(QStringLiteral("STRING"), xyStage)},
                    {QStringLiteral("Position_um"),
                     list(QStringLiteral("DOUBLE"), QJsonArray{position.x, position.y})}});
            }
            if (position.z && !focusStage.isEmpty())
            {
                devices.append(QJsonObject{
                    {QStringLiteral("Device"), scalar(QStringLiteral("STRING"), focusStage)},
                    {QStringLiteral("Position_um"),
                     list(QStringLiteral("DOUBLE"), QJsonArray{*position.z})}});
            }
            stagePositions.append(QJsonObject{
                {QStringLiteral("Label"), scalar(QStringLiteral("STRING"), position.label)},
                {QStringLiteral("DefaultXYStage"), scalar(QStringLiteral("STRING"), xyStage)},
                {QStringLiteral("DefaultZStage"), scalar(QStringLiteral("STRING"), focusStage)},
                {QStringLiteral("GridRow"), scalar(QStringLiteral("INTEGER"), position.gridRow)},
                {QStringLiteral("GridCol"), scalar(QStringLiteral("INTEGER"), position.gridColumn)},
                {QStringLiteral("Properties"),
                 scalar(QStringLiteral("PROPERTY_MAP"), QJsonObject())},
                {QStringLiteral("DevicePositions"), list(QStringLiteral("PROPERTY_MAP"), devices)}});
        }
        const QJsonObject root{
            {QStringLiteral("encoding"), QStringLiteral("UTF-8")},
            {QStringLiteral("format"), QStringLiteral("Micro-Manager Property Map")},
            {QStringLiteral("major_version"), 2},
            {QStringLiteral("minor_version"), 0},
            {QStringLiteral("map"),
             QJsonObject{{QStringLiteral("StagePositions"),
                          list(QStringLiteral("PROPERTY_MAP"), stagePositions)}}}};

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            return fail(errorMessage, file.errorString());
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (!file.commit())
        {
            return fail(errorMessage, file.errorString());
        }
        return true;
    }

    bool PositionListFile::load(const QString& path,
                                QList<ListedPosition>& positions,
                                QString* errorMessage)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return fail(errorMessage, file.errorString());
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (!document.isObject())
        {
            return fail(errorMessage, QStringLiteral("Not a position list: %1").arg(parseError.errorString()));
        }
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("format")).toString() != QStringLiteral("Micro-Manager Property Map"))
        {
            return fail(errorMessage, QStringLiteral("Not a Micro-Manager position list"));
        }
        const QJsonObject stagePositions =
            root.value(QStringLiteral("map")).toObject().value(QStringLiteral("StagePositions")).toObject();
        if (!stagePositions.contains(QStringLiteral("array")))
        {
            return fail(errorMessage, QStringLiteral("The file has no stage positions"));
        }

        QList<ListedPosition> loaded;
        for (const QJsonValue& entry : stagePositions.value(QStringLiteral("array")).toArray())
        {
            const QJsonObject map = entry.toObject();
            const QString xyStage = firstValue(map, QStringLiteral("DefaultXYStage")).toString();
            const QString focusStage = firstValue(map, QStringLiteral("DefaultZStage")).toString();
            ListedPosition position;
            position.label = firstValue(map, QStringLiteral("Label")).toString();
            position.gridRow = firstValue(map, QStringLiteral("GridRow")).toInt();
            position.gridColumn = firstValue(map, QStringLiteral("GridCol")).toInt();
            bool hasXY = false;
            const QJsonArray devices =
                map.value(QStringLiteral("DevicePositions")).toObject().value(QStringLiteral("array")).toArray();
            for (const QJsonValue& deviceValue : devices)
            {
                const QJsonObject device = deviceValue.toObject();
                const QString name = firstValue(device, QStringLiteral("Device")).toString();
                const QJsonArray coordinates =
                    device.value(QStringLiteral("Position_um")).toObject().value(QStringLiteral("array")).toArray();
                if (coordinates.size() == 2 && (name == xyStage || !hasXY))
                {
                    position.x = coordinates.at(0).toDouble();
                    position.y = coordinates.at(1).toDouble();
                    hasXY = true;
                }
                else if (coordinates.size() == 1 && (name == focusStage || !position.z))
                {
                    position.z = coordinates.at(0).toDouble();
                }
            }
            if (!hasXY)
            {
                return fail(errorMessage,
                            QStringLiteral("Position '%1' has no XY coordinates").arg(position.label));
            }
            loaded.push_back(position);
        }
        positions = loaded;
        return true;
    }
}
