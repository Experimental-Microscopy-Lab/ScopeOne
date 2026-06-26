#include "ScopeOneLocalApiServer.h"

#include "scopeone/ScopeOneCore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QEventLoop>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scopeone::ui
{
    namespace
    {
        constexpr quint32 kMaxMessageBytes = 256 * 1024;
        const QString kServerName = QStringLiteral(R"(\\.\pipe\ScopeOne.Api.local)");
        const QString kFrameMappingName = QStringLiteral("ScopeOne.Api.frame");

        // Encodes one JSON object with a little endian size prefix
        QByteArray encodeMessage(const QJsonObject& message)
        {
            const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
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

        // Converts an axis name into a recording axis enum
        scopeone::core::ScopeOneCore::RecordingAxis axisFromName(const QString& name)
        {
            const QString normalized = name.trimmed().toLower();
            if (normalized == QStringLiteral("z"))
            {
                return scopeone::core::ScopeOneCore::RecordingAxis::Z;
            }
            if (normalized == QStringLiteral("xy"))
            {
                return scopeone::core::ScopeOneCore::RecordingAxis::XY;
            }
            return scopeone::core::ScopeOneCore::RecordingAxis::Time;
        }

        // Reads a JSON array as double values
        std::vector<double> doubleArrayFromJson(const QJsonArray& array)
        {
            std::vector<double> values;
            values.reserve(static_cast<size_t>(array.size()));
            for (const QJsonValue& value : array)
            {
                values.push_back(value.toDouble());
            }
            return values;
        }

        // Reads a JSON array as XY positions
        std::vector<QPointF> pointArrayFromJson(const QJsonArray& array)
        {
            std::vector<QPointF> points;
            points.reserve(static_cast<size_t>(array.size()));
            for (const QJsonValue& value : array)
            {
                if (value.isArray())
                {
                    const QJsonArray point = value.toArray();
                    if (point.size() >= 2)
                    {
                        points.emplace_back(point.at(0).toDouble(), point.at(1).toDouble());
                    }
                }
                else if (value.isObject())
                {
                    const QJsonObject point = value.toObject();
                    points.emplace_back(point.value(QStringLiteral("x")).toDouble(),
                                        point.value(QStringLiteral("y")).toDouble());
                }
            }
            return points;
        }

        // Reads a string field or returns the requested default value
        QString stringValueOrDefault(const QJsonObject& object, const QString& key, const QString& defaultValue)
        {
            const QString value = object.value(key).toString().trimmed();
            return value.isEmpty() ? defaultValue : value;
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
    } // namespace

    // Starts the local API pipe server and frame mapping
    ScopeOneLocalApiServer::ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core, QObject* parent)
        : QObject(parent)
          , m_scopeonecore(core)
          , m_server(new QLocalServer(this))
    {
        Q_ASSERT(m_scopeonecore);

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
#endif
    }

    // Releases local API shared frame resources
    ScopeOneLocalApiServer::~ScopeOneLocalApiServer()
    {
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
            sendResponse(socket, processRequest(request));
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
        socket->write(encodeMessage(response));
        socket->flush();
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
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("camera_ids"))
        {
            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
            return response;
        }

        if (type == QStringLiteral("load_config"))
        {
            scopeone::core::ScopeOneCore::LoadConfigResult result;
            QString errorMessage;
            const QString configPath = request.value(QStringLiteral("configPath")).toString().trimmed();
            const bool ok = !configPath.isEmpty()
                && m_scopeonecore->loadConfiguration(configPath, &result, &errorMessage);
            QJsonObject response = makeResponse(type, ok);
            if (ok)
            {
                response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
            }
            else
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to load configuration")
                                    : errorMessage);
            }
            return response;
        }

        if (type == QStringLiteral("unload_config"))
        {
            m_scopeonecore->unloadConfiguration();
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("start_preview"))
        {
            m_scopeonecore->startPreview(request.value(QStringLiteral("camera")).toString(QStringLiteral("All")));
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("stop_preview"))
        {
            m_scopeonecore->stopPreview(request.value(QStringLiteral("camera")).toString(QStringLiteral("All")));
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

        if (type == QStringLiteral("move_xy_relative"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentXYStageDevice());
            const bool ok = m_scopeonecore->moveXYRelative(device,
                                                           request.value(QStringLiteral("dx")).toDouble(),
                                                           request.value(QStringLiteral("dy")).toDouble());
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to move XY stage"));
            }
            return response;
        }

        if (type == QStringLiteral("move_z_relative"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentFocusDevice());
            const bool ok = m_scopeonecore->moveZRelative(device, request.value(QStringLiteral("dz")).toDouble());
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to move Z stage"));
            }
            return response;
        }

        if (type == QStringLiteral("move_xy_to"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentXYStageDevice());
            const bool ok = m_scopeonecore->moveXYTo(device,
                                                     request.value(QStringLiteral("x")).toDouble(),
                                                     request.value(QStringLiteral("y")).toDouble());
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to move XY stage"));
            }
            return response;
        }

        if (type == QStringLiteral("move_z_to"))
        {
            const QString device = stringValueOrDefault(request,
                                                        QStringLiteral("device"),
                                                        m_scopeonecore->currentFocusDevice());
            const bool ok = m_scopeonecore->moveZTo(device, request.value(QStringLiteral("z")).toDouble());
            QJsonObject response = makeResponse(type, ok);
            if (!ok)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to move Z stage"));
            }
            return response;
        }

        if (type == QStringLiteral("record"))
        {
            QString errorMessage;
            const int frameCount = request.value(QStringLiteral("frames")).toInt();
            const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
            const int timeoutMs = request.value(QStringLiteral("timeoutMs")).toInt(120000);
            scopeone::core::ScopeOneCore::RecordingSettings settings;
            settings.format = scopeone::core::RecordingFormat::Tiff;
            settings.streamToDisk = false;
            settings.framesPerBurst = frameCount;
            settings.burstMode = false;
            settings.targetBursts = 1;
            settings.enableCompression = false;
            settings.captureAll = true;
            settings.mdaIntervalMs = request.value(QStringLiteral("mdaIntervalMs")).toDouble(0.0);
            settings.zPositions = doubleArrayFromJson(request.value(QStringLiteral("zPositions")).toArray());
            settings.positions = pointArrayFromJson(request.value(QStringLiteral("positions")).toArray());
            const QJsonArray orderArray = request.value(QStringLiteral("order")).toArray();
            if (!orderArray.isEmpty())
            {
                settings.order.clear();
                for (const QJsonValue& axis : orderArray)
                {
                    settings.order.push_back(axisFromName(axis.toString()));
                }
            }
            const auto session = runRecording(settings, camera, timeoutMs, errorMessage);
            QJsonObject response = makeResponse(type, static_cast<bool>(session));
            if (!session)
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Recording failed")
                                    : errorMessage);
                return response;
            }

            const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_sessions.insert(sessionId, session);
            response.insert(QStringLiteral("sessionId"), sessionId);
            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(session->recordedCameraIds()));
            return response;
        }

        if (type == QStringLiteral("session_info"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const auto session = m_sessions.value(sessionId);
            QJsonObject response = makeResponse(type, static_cast<bool>(session));
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
                return response;
            }

            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(session->recordedCameraIds()));
            response.insert(QStringLiteral("frameCount"), session->frameCount());
            QJsonObject counts;
            for (const QString& cameraId : session->recordedCameraIds())
            {
                const auto* frames = session->framesForCamera(cameraId);
                counts.insert(cameraId, frames ? static_cast<int>(frames->size()) : 0);
            }
            response.insert(QStringLiteral("frameCounts"), counts);
            return response;
        }

        if (type == QStringLiteral("session_frame"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            const int index = request.value(QStringLiteral("index")).toInt(-1);
            const auto session = m_sessions.value(sessionId);
            QJsonObject response = makeResponse(type, false);
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
                return response;
            }
            const auto* frames = session->framesForCamera(cameraId);
            if (!frames || index < 0 || index >= static_cast<int>(frames->size()))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Frame index out of range"));
                return response;
            }

            QString errorMessage;
            if (!exportFrameToSharedMemory(frames->at(static_cast<size_t>(index)), errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to export frame")
                                    : errorMessage);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            return response;
        }

        if (type == QStringLiteral("session_save"))
        {
            const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
            const auto session = m_sessions.value(sessionId);
            QJsonObject response = makeResponse(type, false);
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
                return response;
            }

            auto capturePlan = session->capturePlan();
            capturePlan.saveDir = request.value(QStringLiteral("saveDir")).toString().trimmed();
            capturePlan.baseName = request.value(QStringLiteral("baseName")).toString().trimmed();
            const QString formatName = request.value(QStringLiteral("format")).toString(QStringLiteral("tiff")).
                                               trimmed().toLower();
            capturePlan.format = (formatName == QStringLiteral("binary") || formatName == QStringLiteral("bin"))
                                     ? scopeone::core::RecordingFormat::Binary
                                     : scopeone::core::RecordingFormat::Tiff;
            capturePlan.enableCompression = request.value(QStringLiteral("compression")).toBool(false);
            capturePlan.compressionLevel = request.value(QStringLiteral("compressionLevel")).toInt(6);
            session->setCapturePlan(capturePlan);

            const QString saveResult = m_scopeonecore->saveRecordingSession(session);
            if (saveResult.startsWith(QStringLiteral("Error:"), Qt::CaseInsensitive))
            {
                response.insert(QStringLiteral("error"), saveResult);
                return response;
            }

            response = makeResponse(type, true);
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
            response.insert(QStringLiteral("paths"), paths);
            return response;
        }

        QJsonObject response = makeResponse(type, false);
        response.insert(QStringLiteral("error"), QStringLiteral("Unknown request type"));
        return response;
    }

    // Runs a blocking API recording and returns the captured session
    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> ScopeOneLocalApiServer::runRecording(
        const scopeone::core::ScopeOneCore::RecordingSettings& settings,
        const QString& cameraIdOrAll,
        int timeoutMs,
        QString& errorMessage)
    {
        if (settings.framesPerBurst <= 0)
        {
            errorMessage = QStringLiteral("frames must be > 0");
            return {};
        }

        const QStringList activeCameraIds = resolveCameraIds(m_scopeonecore, cameraIdOrAll);
        if (activeCameraIds.isEmpty())
        {
            errorMessage = QStringLiteral("No matching camera available for recording: %1").arg(cameraIdOrAll);
            return {};
        }
        const bool useMda = !settings.positions.empty() || !settings.zPositions.empty();
        bool timedOut = false;

        std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> recordedSession;
        const QMetaObject::Connection connection = QObject::connect(
            m_scopeonecore,
            &scopeone::core::ScopeOneCore::recordingStopped,
            m_scopeonecore,
            [&recordedSession](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
            {
                recordedSession = session;
            });

        try
        {
            if (!useMda)
            {
                m_scopeonecore->startPreview(cameraIdOrAll);
            }

            if (!m_scopeonecore->startRecording(settings, activeCameraIds))
            {
                errorMessage = useMda
                                   ? QStringLiteral("Failed to start MDA recording")
                                   : QStringLiteral("Failed to start preview-frame recording");
                QObject::disconnect(connection);
                if (!useMda)
                {
                    m_scopeonecore->stopPreview(cameraIdOrAll);
                }
                return {};
            }

            QElapsedTimer timer;
            timer.start();
            const int waitTimeoutMs = timeoutMs > 0 ? timeoutMs : 120000;
            while (m_scopeonecore->isRecording() && timer.elapsed() < waitTimeoutMs)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(10);
            }

            if (m_scopeonecore->isRecording())
            {
                timedOut = true;
                m_scopeonecore->stopRecording();
            }

            for (int i = 0; i < 20 && !recordedSession; ++i)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(10);
            }

            if (!useMda)
            {
                m_scopeonecore->stopPreview(cameraIdOrAll);
            }
            QObject::disconnect(connection);
        }
        catch (...)
        {
            if (m_scopeonecore->isRecording())
            {
                m_scopeonecore->stopRecording();
            }
            if (!useMda)
            {
                m_scopeonecore->stopPreview(cameraIdOrAll);
            }
            QObject::disconnect(connection);
            throw;
        }

        if (!recordedSession)
        {
            errorMessage = timedOut
                               ? QStringLiteral("Recording timed out after %1 ms before session data was returned")
                                     .arg(timeoutMs > 0 ? timeoutMs : 120000)
                               : QStringLiteral("Recording finished but no session data was returned");
            return {};
        }
        if (!recordedSession->hasAnyFrames())
        {
            if (timedOut)
            {
                errorMessage = useMda
                                   ? QStringLiteral("MDA recording timed out and captured no frames")
                                   : QStringLiteral("Preview-frame recording timed out and captured no frames");
            }
            else
            {
                errorMessage = useMda
                                   ? QStringLiteral(
                                       "MDA recording finished but captured no frames. Check stage devices, camera snap support, and ScopeOne logs for MDA errors")
                                   : QStringLiteral(
                                       "Preview-frame recording finished but captured no frames. Check that preview is producing raw frames for the selected camera");
            }
            return {};
        }
        return recordedSession;
    }

    // Exports one captured frame to the shared frame mapping
    bool ScopeOneLocalApiServer::exportFrameToSharedMemory(
        const scopeone::core::ScopeOneCore::RecordingFrame& frame,
        QString& errorMessage)
    {
        if (!m_frameMappingHandle || !m_frameMappingView)
        {
            errorMessage = QStringLiteral("Frame mapping is not available");
            return false;
        }
        if (frame.rawData.size() <= 0
            || frame.rawData.size() > scopeone::core::kSharedFrameMaxBytes)
        {
            errorMessage = QStringLiteral("Frame payload is invalid");
            return false;
        }

        std::memcpy(m_frameMappingView, &frame.header, sizeof(frame.header));
        std::memcpy(m_frameMappingView + scopeone::core::kSharedFrameHeaderSize,
                    frame.rawData.constData(),
                    static_cast<size_t>(frame.rawData.size()));
        return true;
    }
} // namespace scopeone::ui
