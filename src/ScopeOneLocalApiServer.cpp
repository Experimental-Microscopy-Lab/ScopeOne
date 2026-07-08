#include "ScopeOneLocalApiServer.h"

#include "ImageMarkupModel.h"
#include "PreviewWidget.h"
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
    namespace
    {
        constexpr quint32 kMaxMessageBytes = 256 * 1024;
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
        QJsonObject markupToJson(const ImageMarkupModel::Markup& markup)
        {
            QJsonObject object;
            object.insert(QStringLiteral("id"), markup.id);
            object.insert(QStringLiteral("type"), ImageMarkupModel::typeName(markup.type));
            object.insert(QStringLiteral("role"), ImageMarkupModel::roleName(markup.role));
            object.insert(QStringLiteral("layerKey"), markup.layerKey);
            object.insert(QStringLiteral("label"), markup.label);
            if (markup.type == ImageMarkupModel::MarkupType::Line)
            {
                object.insert(QStringLiteral("x1"), markup.start.x());
                object.insert(QStringLiteral("y1"), markup.start.y());
                object.insert(QStringLiteral("x2"), markup.end.x());
                object.insert(QStringLiteral("y2"), markup.end.y());
            }
            else if (markup.type == ImageMarkupModel::MarkupType::Rect)
            {
                object.insert(QStringLiteral("x"), markup.rect.x());
                object.insert(QStringLiteral("y"), markup.rect.y());
                object.insert(QStringLiteral("width"), markup.rect.width());
                object.insert(QStringLiteral("height"), markup.rect.height());
            }
            return object;
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

        // Reads one required integer field
        bool intField(const QJsonObject& object, const QString& key, int& value)
        {
            const QJsonValue field = object.value(key);
            if (!field.isDouble())
            {
                return false;
            }
            value = field.toInt();
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
            return {};
        }

        void applySaveRequest(
            const QJsonObject& request,
            scopeone::core::ScopeOneCore::RecordingCapturePlanData& capturePlan)
        {
            capturePlan.saveDir = request.value(QStringLiteral("saveDir")).toString().trimmed();
            capturePlan.baseName = request.value(QStringLiteral("baseName")).toString().trimmed();
            const QString formatName = request.value(QStringLiteral("format"))
                                           .toString(QStringLiteral("tiff"))
                                           .trimmed()
                                           .toLower();
            capturePlan.format = (formatName == QStringLiteral("binary") || formatName == QStringLiteral("bin"))
                                     ? scopeone::core::RecordingFormat::Binary
                                     : scopeone::core::RecordingFormat::Tiff;
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

        bool processAndPublishExternalApiFrame(scopeone::core::ScopeOneCore* core,
                                               const scopeone::core::ImageFrame& frame,
                                               const QJsonObject& request,
                                               ProcessFrameRequestResult& result,
                                               QString& errorMessage)
        {
            result = processFrameFromRequest(core, frame, request, errorMessage);
            if (!result.frame.isValid())
            {
                if (errorMessage.isEmpty())
                {
                    errorMessage = QStringLiteral("Processing produced no valid frame");
                }
                return false;
            }

            result.frame = publishExternalApiFrame(core, result.frame, errorMessage);
            return result.frame.isValid();
        }
    } // namespace

    // Starts the local API pipe server and frame mapping
    ScopeOneLocalApiServer::ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                                   PreviewWidget* previewWidget,
                                                   ImageMarkupModel* markupModel,
                                                   QObject* parent)
        : QObject(parent)
          , m_scopeonecore(core)
          , m_previewWidget(previewWidget)
          , m_markupModel(markupModel)
          , m_server(new QLocalServer(this))
    {
        if (!core)
        {
            qFatal("ScopeOneLocalApiServer requires ScopeOneCore");
        }

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

        if (type == QStringLiteral("list_layers"))
        {
            const QStringList selectedLayerKeys = m_previewWidget->selectedLayerKeys();
            QJsonArray layers;
            for (const QString& layerKey : m_previewWidget->availableLayerKeys())
            {
                QJsonObject layer;
                layer.insert(QStringLiteral("layerKey"), layerKey);
                layer.insert(QStringLiteral("sourceId"),
                             scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey));
                layer.insert(QStringLiteral("name"), m_previewWidget->layerName(layerKey));
                layer.insert(QStringLiteral("info"), m_previewWidget->layerInfoText(layerKey));
                layer.insert(QStringLiteral("selected"), selectedLayerKeys.contains(layerKey));
                if (scopeone::core::ScopeOneCore::isRawLayerKey(layerKey))
                {
                    layer.insert(QStringLiteral("kind"), QStringLiteral("raw"));
                }
                else if (scopeone::core::ScopeOneCore::isProcessedLayerKey(layerKey))
                {
                    layer.insert(QStringLiteral("kind"), QStringLiteral("processed"));
                }
                else if (scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey))
                {
                    layer.insert(QStringLiteral("kind"), QStringLiteral("static"));
                }
                layers.append(layer);
            }

            QJsonObject response = makeResponse(type, true);
            response.insert(QStringLiteral("layers"), layers);
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
            if (layerKey.isEmpty() || !m_previewWidget->availableLayerKeys().contains(layerKey))
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

            const QString id = m_markupModel->createLine(
                layerKey,
                QPoint(x1, y1),
                QPoint(x2, y2),
                request.value(QStringLiteral("label")).toString());
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
            if (layerKey.isEmpty() || !m_previewWidget->availableLayerKeys().contains(layerKey))
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

            const QString id = m_markupModel->createRect(
                layerKey,
                QRect(x, y, width, height),
                request.value(QStringLiteral("label")).toString());
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
            if (!layerKey.isEmpty() && !m_previewWidget->availableLayerKeys().contains(layerKey))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            QJsonArray markups;
            for (const ImageMarkupModel::Markup& markup : m_markupModel->markups(layerKey))
            {
                markups.append(markupToJson(markup));
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("markups"), markups);
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
            ImageMarkupModel::Markup markup;
            if (!m_markupModel->findMarkup(markupId, markup) || !m_markupModel->remove(markupId))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Unknown markup"));
                return response;
            }
            if (markup.role == ImageMarkupModel::MarkupRole::CrossSection)
            {
                m_scopeonecore->clearLineProfile();
            }
            return makeResponse(type, true);
        }

        if (type == QStringLiteral("clear_markups"))
        {
            const QString layerKey = request.value(QStringLiteral("layerKey")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (!layerKey.isEmpty() && !m_previewWidget->availableLayerKeys().contains(layerKey))
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing or unknown layerKey"));
                return response;
            }

            const bool clearsCrossSection = m_markupModel->hasRole(
                ImageMarkupModel::MarkupRole::CrossSection,
                layerKey);
            m_markupModel->clear(layerKey);
            if (clearsCrossSection)
            {
                m_scopeonecore->clearLineProfile();
            }
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
            response.insert(QStringLiteral("frameCount"), session->recordedFrameCount());
            QJsonObject counts;
            for (const QString& cameraId : session->recordedCameraIds())
            {
                counts.insert(cameraId, session->recordedFrameCount(cameraId));
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
            const scopeone::core::ImageFrame frame = m_scopeonecore->sessionFrameAt(session, cameraId, index);
            if (!frame.isValid())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Frame index out of range"));
                return response;
            }

            QString errorMessage;
            const scopeone::core::ImageFrame graphFrame =
                publishExternalApiFrame(m_scopeonecore, frame, errorMessage);
            if (!graphFrame.isValid())
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }

            scopeone::core::ImageFrame exportedFrame;
            if (!exportFrameToSharedMemory(graphFrame, exportedFrame, errorMessage))
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
            addFrameMetadata(response, exportedFrame);
            return response;
        }

        if (type == QStringLiteral("latest_raw_frame"))
        {
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            if (cameraId.isEmpty())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Missing camera"));
                return response;
            }

            const scopeone::core::ImageFrame frame =
                m_scopeonecore->graphFrame(scopeone::core::ScopeOneCore::rawLayerKey(cameraId));
            if (!frame.isValid())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("No latest raw frame for camera"));
                return response;
            }

            QString errorMessage;
            scopeone::core::ImageFrame exportedFrame;
            if (!exportFrameToSharedMemory(frame, exportedFrame, errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to export latest raw frame")
                                    : errorMessage);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            addFrameMetadata(response, exportedFrame);
            return response;
        }

        if (type == QStringLiteral("session_process_frame"))
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

            const scopeone::core::ImageFrame frame = m_scopeonecore->sessionFrameAt(session, cameraId, index);
            if (!frame.isValid())
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Frame index out of range"));
                return response;
            }

            QString errorMessage;
            ProcessFrameRequestResult processedResult;
            if (!processAndPublishExternalApiFrame(m_scopeonecore,
                                                   frame,
                                                   request,
                                                   processedResult,
                                                   errorMessage))
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }

            scopeone::core::ImageFrame exportedFrame;
            if (!exportFrameToSharedMemory(processedResult.frame, exportedFrame, errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to export processed frame")
                                    : errorMessage);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            addFrameMetadata(response, exportedFrame);
            addProcessingMetadata(response, processedResult);
            return response;
        }

        if (type == QStringLiteral("process_frame_mapping"))
        {
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);

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

            ProcessFrameRequestResult processedResult;
            if (!processAndPublishExternalApiFrame(m_scopeonecore,
                                                   graphFrame,
                                                   request,
                                                   processedResult,
                                                   errorMessage))
            {
                response.insert(QStringLiteral("error"), errorMessage);
                return response;
            }

            scopeone::core::ImageFrame exportedFrame;
            if (!exportFrameToSharedMemory(processedResult.frame, exportedFrame, errorMessage))
            {
                response.insert(QStringLiteral("error"),
                                errorMessage.isEmpty()
                                    ? QStringLiteral("Failed to export processed frame")
                                    : errorMessage);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("mappingName"), kFrameMappingName);
            response.insert(QStringLiteral("mappingSize"),
                            static_cast<int>(scopeone::core::kSharedFrameHeaderSize
                                + scopeone::core::kSharedFrameMaxBytes));
            addFrameMetadata(response, exportedFrame);
            addProcessingMetadata(response, processedResult);
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

        if (type == QStringLiteral("save_frame_mapping"))
        {
            const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
            QJsonObject response = makeResponse(type, false);
            const QString saveError = saveRequestError(request);
            if (!saveError.isEmpty())
            {
                response.insert(QStringLiteral("error"), saveError);
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

            scopeone::core::ScopeOneCore::RecordingCapturePlanData capturePlan;
            capturePlan.cameraIds.append(graphFrame.cameraId);
            capturePlan.streamToDisk = false;
            capturePlan.captureAll = false;
            applySaveRequest(request, capturePlan);
            QList<scopeone::core::ImageFrame> frames;
            frames.append(graphFrame);
            auto session = m_scopeonecore->createFrameSession(
                frames,
                capturePlan);
            if (!session)
            {
                response.insert(QStringLiteral("error"), QStringLiteral("Failed to create frame session"));
                return response;
            }

            const QString saveResult = m_scopeonecore->saveRecordingSession(session);
            if (saveResult.startsWith(QStringLiteral("Error:"), Qt::CaseInsensitive))
            {
                response.insert(QStringLiteral("error"), saveResult);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("paths"), savedFramePaths(session));
            addFrameMetadata(response, graphFrame);
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
            const QString saveError = saveRequestError(request);
            if (!saveError.isEmpty())
            {
                response.insert(QStringLiteral("error"), saveError);
                return response;
            }

            auto capturePlan = session->capturePlan();
            applySaveRequest(request, capturePlan);

            const QString saveResult = m_scopeonecore->saveRecordingSession(session, capturePlan);
            if (saveResult.startsWith(QStringLiteral("Error:"), Qt::CaseInsensitive))
            {
                response.insert(QStringLiteral("error"), saveResult);
                return response;
            }

            response = makeResponse(type, true);
            response.insert(QStringLiteral("paths"), savedFramePaths(session));
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
        if (recordedSession->streamedToDisk() && !recordedSession->isSaved())
        {
            errorMessage = recordedSession->saveMessage().isEmpty()
                               ? QStringLiteral("Recording writer failed")
                               : recordedSession->saveMessage();
            return {};
        }
        if (!recordedSession->hasRecordedOutput())
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
                                                             QString& errorMessage) const
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
        return true;
    }
} // namespace scopeone::ui
