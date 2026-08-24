#pragma once

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <memory>

#include "scopeone/ScopeOneCore.h"

class QLocalServer;
class QLocalSocket;

namespace scopeone::ui
{
    class PreviewWidget;
    class ImageWorkspace;

    class ScopeOneLocalApiServer : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                        PreviewWidget* previewWidget,
                                        ImageWorkspace* imageWorkspace,
                                        QObject* parent = nullptr);
        ~ScopeOneLocalApiServer() override;

    private:
        void handleNewConnection();
        void handleSocketReadyRead(QLocalSocket* socket);
        void handleSocketDisconnected(QLocalSocket* socket);
        void sendResponse(QLocalSocket* socket, const QJsonObject& response);
        void sendRequestResponse(QLocalSocket* socket,
                                 QJsonObject response,
                                 const QJsonValue& requestId);
        bool processAsyncRequest(QLocalSocket* socket,
                                 const QJsonObject& request,
                                 const QJsonValue& requestId);
        QJsonObject processRequest(const QJsonObject& request);
        scopeone::core::ExperimentDocument createExperimentDocument();
        QJsonObject experimentStatusResponse(const QString& type,
                                             const QString& experimentId) const;
        bool exportFrameToSharedMemory(const scopeone::core::ImageFrame& frame,
                                       scopeone::core::ImageFrame& exportedFrame,
                                       QString& errorMessage);
        bool importFrameFromSharedMemory(const QString& cameraId,
                                         scopeone::core::ImageFrame& frame,
                                         QString& errorMessage);

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        PreviewWidget* m_previewWidget{nullptr};
        ImageWorkspace* m_imageWorkspace{nullptr};
        scopeone::core::ImageSceneModel* m_sceneModel{nullptr};
        QLocalServer* m_server{nullptr};
        QHash<QLocalSocket*, QByteArray> m_readBuffers;
        QThreadPool m_taskPool;
        QString m_frameMappingCameraId;

        void* m_frameMappingHandle{nullptr};
        uchar* m_frameMappingView{nullptr};
        int m_frameShmFd{-1};
    };
} // namespace scopeone::ui
