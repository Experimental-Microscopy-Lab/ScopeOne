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

#include <windows.h>

namespace scopeone::ui {

namespace {

constexpr quint32 kMaxMessageBytes = 256 * 1024;
const QString kServerName = QStringLiteral(R"(\\.\pipe\ScopeOne.Api.local)");
const QString kFrameMappingName = QStringLiteral("ScopeOne.Api.frame");

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

DecodeResult tryDecodeMessage(QByteArray& buffer, QJsonObject& message)
{
    if (buffer.size() < static_cast<int>(sizeof(quint32))) {
        return DecodeResult::Incomplete;
    }

    const quint32 payloadSize =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData()));
    if (payloadSize == 0 || payloadSize > kMaxMessageBytes) {
        buffer.clear();
        return DecodeResult::Error;
    }

    const int frameSize = static_cast<int>(sizeof(quint32) + payloadSize);
    if (buffer.size() < frameSize) {
        return DecodeResult::Incomplete;
    }

    const QByteArray payload = buffer.mid(static_cast<int>(sizeof(quint32)), static_cast<int>(payloadSize));
    buffer.remove(0, frameSize);

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return DecodeResult::Error;
    }

    message = document.object();
    return DecodeResult::Complete;
}

QJsonObject makeResponse(const QString& type, bool ok)
{
    QJsonObject response;
    response.insert(QStringLiteral("type"), type);
    response.insert(QStringLiteral("ok"), ok);
    return response;
}

QStringList resolveCameraIds(scopeone::core::ScopeOneCore* core, const QString& cameraIdOrAll)
{
    const QStringList availableCameraIds = core->cameraIds();
    if (availableCameraIds.isEmpty()) {
        return {};
    }

    const QString target = cameraIdOrAll.trimmed();
    if (target.isEmpty()) {
        return {};
    }
    if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0) {
        return availableCameraIds;
    }
    if (!availableCameraIds.contains(target)) {
        return {};
    }
    return QStringList{target};
}

} // namespace

ScopeOneLocalApiServer::ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core, QObject* parent)
    : QObject(parent)
    , m_scopeonecore(core)
    , m_server(new QLocalServer(this))
{
    Q_ASSERT(m_scopeonecore);

    QLocalServer::removeServer(kServerName);
    connect(m_server, &QLocalServer::newConnection,
            this, &ScopeOneLocalApiServer::handleNewConnection);

    if (!m_server->listen(kServerName)) {
        qWarning().noquote()
            << QStringLiteral("ScopeOne local API server failed to listen on '%1': %2. Another ScopeOne instance may already be running.")
                  .arg(kServerName, m_server->errorString());
    } else {
        qInfo().noquote()
            << QStringLiteral("ScopeOne local API server listening on '%1'")
                  .arg(m_server->fullServerName());
    }

    const DWORD mappingSize = static_cast<DWORD>(
        scopeone::core::kSharedFrameHeaderSize + scopeone::core::kSharedFrameMaxBytes);
    m_frameMappingHandle = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                              nullptr,
                                              PAGE_READWRITE,
                                              0,
                                              mappingSize,
                                              reinterpret_cast<LPCWSTR>(kFrameMappingName.utf16()));
    if (!m_frameMappingHandle) {
        qWarning().noquote() << QStringLiteral("ScopeOne API frame mapping create failed");
        return;
    }
    m_frameMappingView = static_cast<uchar*>(
        MapViewOfFile(m_frameMappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, mappingSize));
    if (!m_frameMappingView) {
        qWarning().noquote() << QStringLiteral("ScopeOne API frame mapping view failed");
        CloseHandle(m_frameMappingHandle);
        m_frameMappingHandle = nullptr;
    }
}

ScopeOneLocalApiServer::~ScopeOneLocalApiServer()
{
    if (m_frameMappingView) {
        UnmapViewOfFile(m_frameMappingView);
        m_frameMappingView = nullptr;
    }
    if (m_frameMappingHandle) {
        CloseHandle(m_frameMappingHandle);
        m_frameMappingHandle = nullptr;
    }
}

void ScopeOneLocalApiServer::handleNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket]() { handleSocketReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected,
                this, [this, socket]() { handleSocketDisconnected(socket); });
    }
}

void ScopeOneLocalApiServer::handleSocketReadyRead(QLocalSocket* socket)
{
    QByteArray& buffer = m_readBuffers[socket];
    buffer += socket->readAll();

    while (true) {
        QJsonObject request;
        const DecodeResult result = tryDecodeMessage(buffer, request);
        if (result == DecodeResult::Incomplete) {
            return;
        }
        if (result == DecodeResult::Error) {
            sendResponse(socket, makeResponse(QStringLiteral("error"), false));
            socket->disconnectFromServer();
            return;
        }
        sendResponse(socket, processRequest(request));
    }
}

void ScopeOneLocalApiServer::handleSocketDisconnected(QLocalSocket* socket)
{
    m_readBuffers.remove(socket);
    socket->deleteLater();
}

void ScopeOneLocalApiServer::sendResponse(QLocalSocket* socket, const QJsonObject& response)
{
    socket->write(encodeMessage(response));
    socket->flush();
}

QJsonObject ScopeOneLocalApiServer::processRequest(const QJsonObject& request)
{
    const QString type = request.value(QStringLiteral("type")).toString().trimmed();
    if (type.isEmpty()) {
        QJsonObject response = makeResponse(QStringLiteral("error"), false);
        response.insert(QStringLiteral("error"), QStringLiteral("Missing request type"));
        return response;
    }

    if (type == QStringLiteral("ping")) {
        return makeResponse(type, true);
    }

    if (type == QStringLiteral("camera_ids")) {
        QJsonObject response = makeResponse(type, true);
        response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
        return response;
    }

    if (type == QStringLiteral("load_config")) {
        scopeone::core::ScopeOneCore::LoadConfigResult result;
        QString errorMessage;
        const QString configPath = request.value(QStringLiteral("configPath")).toString().trimmed();
        const bool ok = !configPath.isEmpty()
            && m_scopeonecore->loadConfiguration(configPath, &result, &errorMessage);
        QJsonObject response = makeResponse(type, ok);
        if (ok) {
            response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(m_scopeonecore->cameraIds()));
        } else {
            response.insert(QStringLiteral("error"),
                            errorMessage.isEmpty()
                                ? QStringLiteral("Failed to load configuration")
                                : errorMessage);
        }
        return response;
    }

    if (type == QStringLiteral("unload_config")) {
        m_scopeonecore->unloadConfiguration();
        return makeResponse(type, true);
    }

    if (type == QStringLiteral("start_preview")) {
        m_scopeonecore->startPreview(request.value(QStringLiteral("camera")).toString(QStringLiteral("All")));
        return makeResponse(type, true);
    }

    if (type == QStringLiteral("stop_preview")) {
        m_scopeonecore->stopPreview(request.value(QStringLiteral("camera")).toString(QStringLiteral("All")));
        return makeResponse(type, true);
    }

    if (type == QStringLiteral("record")) {
        QString errorMessage;
        const int frameCount = request.value(QStringLiteral("frames")).toInt();
        const QString camera = request.value(QStringLiteral("camera")).toString(QStringLiteral("All"));
        const int timeoutMs = request.value(QStringLiteral("timeoutMs")).toInt(120000);
        const auto session = runRecording(frameCount, camera, timeoutMs, errorMessage);
        QJsonObject response = makeResponse(type, static_cast<bool>(session));
        if (!session) {
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

    if (type == QStringLiteral("session_info")) {
        const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
        const auto session = m_sessions.value(sessionId);
        QJsonObject response = makeResponse(type, static_cast<bool>(session));
        if (!session) {
            response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
            return response;
        }

        response.insert(QStringLiteral("cameraIds"), QJsonArray::fromStringList(session->recordedCameraIds()));
        response.insert(QStringLiteral("frameCount"), session->frameCount());
        QJsonObject counts;
        for (const QString& cameraId : session->recordedCameraIds()) {
            const auto* frames = session->framesForCamera(cameraId);
            counts.insert(cameraId, frames ? static_cast<int>(frames->size()) : 0);
        }
        response.insert(QStringLiteral("frameCounts"), counts);
        return response;
    }

    if (type == QStringLiteral("session_frame")) {
        const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
        const QString cameraId = request.value(QStringLiteral("camera")).toString().trimmed();
        const int index = request.value(QStringLiteral("index")).toInt(-1);
        const auto session = m_sessions.value(sessionId);
        QJsonObject response = makeResponse(type, false);
        if (!session) {
            response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
            return response;
        }
        const auto* frames = session->framesForCamera(cameraId);
        if (!frames || index < 0 || index >= static_cast<int>(frames->size())) {
            response.insert(QStringLiteral("error"), QStringLiteral("Frame index out of range"));
            return response;
        }

        QString errorMessage;
        if (!exportFrameToSharedMemory(frames->at(static_cast<size_t>(index)), errorMessage)) {
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

    if (type == QStringLiteral("session_save")) {
        const QString sessionId = request.value(QStringLiteral("sessionId")).toString().trimmed();
        const auto session = m_sessions.value(sessionId);
        QJsonObject response = makeResponse(type, false);
        if (!session) {
            response.insert(QStringLiteral("error"), QStringLiteral("Unknown session"));
            return response;
        }

        auto capturePlan = session->capturePlan();
        capturePlan.saveDir = request.value(QStringLiteral("saveDir")).toString().trimmed();
        capturePlan.baseName = request.value(QStringLiteral("baseName")).toString().trimmed();
        const QString formatName = request.value(QStringLiteral("format")).toString(QStringLiteral("tiff")).trimmed().toLower();
        capturePlan.format = (formatName == QStringLiteral("binary") || formatName == QStringLiteral("bin"))
            ? scopeone::core::RecordingFormat::Binary
            : scopeone::core::RecordingFormat::Tiff;
        capturePlan.enableCompression = request.value(QStringLiteral("compression")).toBool(false);
        capturePlan.compressionLevel = request.value(QStringLiteral("compressionLevel")).toInt(6);
        session->setCapturePlan(capturePlan);

        const QString saveResult = m_scopeonecore->saveRecordingSession(session);
        if (saveResult.startsWith(QStringLiteral("Error:"), Qt::CaseInsensitive)) {
            response.insert(QStringLiteral("error"), saveResult);
            return response;
        }

        response = makeResponse(type, true);
        QJsonArray paths;
        const auto& files = session->outputFiles();
        for (const QString& cameraId : session->recordedCameraIds()) {
            const auto it = files.constFind(cameraId);
            if (it != files.constEnd() && !it.value().rawPath.isEmpty()) {
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

std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> ScopeOneLocalApiServer::runRecording(
    int frameCount,
    const QString& cameraIdOrAll,
    int timeoutMs,
    QString& errorMessage)
{
    if (frameCount <= 0) {
        errorMessage = QStringLiteral("frames must be > 0");
        return {};
    }

    const QStringList activeCameraIds = resolveCameraIds(m_scopeonecore, cameraIdOrAll);
    if (activeCameraIds.isEmpty()) {
        errorMessage = QStringLiteral("No cameras available");
        return {};
    }

    scopeone::core::ScopeOneCore::RecordingSettings settings;
    settings.format = scopeone::core::RecordingFormat::Tiff;
    settings.streamToDisk = false;
    settings.framesPerBurst = frameCount;
    settings.burstMode = false;
    settings.targetBursts = 1;
    settings.enableCompression = false;
    settings.captureAll = true;

    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> recordedSession;
    const QMetaObject::Connection connection = QObject::connect(
        m_scopeonecore,
        &scopeone::core::ScopeOneCore::recordingStopped,
        m_scopeonecore,
        [&recordedSession](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            recordedSession = session;
        });

    try {
        m_scopeonecore->startPreview(cameraIdOrAll);

        if (!m_scopeonecore->startRecording(settings, activeCameraIds)) {
            errorMessage = QStringLiteral("Failed to start recording");
            QObject::disconnect(connection);
            m_scopeonecore->stopPreview(cameraIdOrAll);
            return {};
        }

        QElapsedTimer timer;
        timer.start();
        const int waitTimeoutMs = timeoutMs > 0 ? timeoutMs : 120000;
        while (m_scopeonecore->isRecording() && timer.elapsed() < waitTimeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }

        if (m_scopeonecore->isRecording()) {
            m_scopeonecore->stopRecording();
        }

        for (int i = 0; i < 20 && !recordedSession; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }

        m_scopeonecore->stopPreview(cameraIdOrAll);
        QObject::disconnect(connection);
    } catch (...) {
        if (m_scopeonecore->isRecording()) {
            m_scopeonecore->stopRecording();
        }
        m_scopeonecore->stopPreview(cameraIdOrAll);
        QObject::disconnect(connection);
        throw;
    }

    if (!recordedSession) {
        errorMessage = QStringLiteral("Recording finished but no session data was returned");
        return {};
    }
    if (!recordedSession->hasAnyFrames()) {
        errorMessage = QStringLiteral("Recording finished but captured no frames");
        return {};
    }
    return recordedSession;
}

bool ScopeOneLocalApiServer::exportFrameToSharedMemory(
    const scopeone::core::ScopeOneCore::RecordingFrame& frame,
    QString& errorMessage)
{
    if (!m_frameMappingHandle || !m_frameMappingView) {
        errorMessage = QStringLiteral("Frame mapping is not available");
        return false;
    }
    if (frame.rawData.size() <= 0
        || frame.rawData.size() > scopeone::core::kSharedFrameMaxBytes) {
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
