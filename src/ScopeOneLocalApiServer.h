#pragma once

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <memory>

#include "scopeone/ScopeOneCore.h"

class QLocalServer;
class QLocalSocket;

namespace scopeone::ui
{
    class ScopeOneLocalApiServer : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core, QObject* parent = nullptr);
        ~ScopeOneLocalApiServer() override;

    private:
        void handleNewConnection();
        void handleSocketReadyRead(QLocalSocket* socket);
        void handleSocketDisconnected(QLocalSocket* socket);
        void sendResponse(QLocalSocket* socket, const QJsonObject& response);
        QJsonObject processRequest(const QJsonObject& request);
        std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> runRecording(
            const scopeone::core::ScopeOneCore::RecordingSettings& settings,
            const QString& cameraIdOrAll,
            int timeoutMs,
            QString& errorMessage);
        bool exportFrameToSharedMemory(const scopeone::core::ScopeOneCore::RecordingFrame& frame,
                                       QString& errorMessage);

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QLocalServer* m_server{nullptr};
        QHash<QLocalSocket*, QByteArray> m_readBuffers;
        QHash<QString, std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> m_sessions;

        void* m_frameMappingHandle{nullptr};
        uchar* m_frameMappingView{nullptr};
        int m_frameShmFd{-1};
    };
} // namespace scopeone::ui
