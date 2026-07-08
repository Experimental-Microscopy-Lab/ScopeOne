#pragma once

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>

#include "scopeone/ScopeOneCore.h"

class QLocalServer;
class QLocalSocket;

namespace scopeone::ui
{
    class ImageMarkupModel;
    class PreviewWidget;

    class ScopeOneLocalApiServer : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                        PreviewWidget* previewWidget,
                                        ImageMarkupModel* markupModel,
                                        QObject* parent = nullptr);
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
        bool exportFrameToSharedMemory(const scopeone::core::ImageFrame& frame,
                                       scopeone::core::ImageFrame& exportedFrame,
                                       QString& errorMessage);
        bool importFrameFromSharedMemory(const QString& cameraId,
                                         scopeone::core::ImageFrame& frame,
                                         QString& errorMessage) const;

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        PreviewWidget* m_previewWidget{nullptr};
        ImageMarkupModel* m_markupModel{nullptr};
        QLocalServer* m_server{nullptr};
        QHash<QLocalSocket*, QByteArray> m_readBuffers;
        QHash<QString, std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> m_sessions;
        QString m_frameMappingCameraId;

        void* m_frameMappingHandle{nullptr};
        uchar* m_frameMappingView{nullptr};
        int m_frameShmFd{-1};
    };
} // namespace scopeone::ui
