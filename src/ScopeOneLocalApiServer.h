#pragma once

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <functional>
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
        using ResponseCallback = std::function<void(QJsonObject)>;

        explicit ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                        PreviewWidget* previewWidget,
                                        ImageWorkspace* imageWorkspace,
                                        QObject* parent = nullptr);
        ~ScopeOneLocalApiServer() override;

        QJsonObject processRequest(const QJsonObject& request);
        void dispatchRequest(const QJsonObject& request, ResponseCallback callback);

    private:
        void handleNewConnection();
        void handleSocketReadyRead(QLocalSocket* socket);
        void handleSocketDisconnected(QLocalSocket* socket);
        void sendResponse(QLocalSocket* socket, const QJsonObject& response);
        void logRequest(const QString& origin, const QJsonObject& request, const QJsonObject& response);
        QJsonObject updateSubscription(QLocalSocket* socket, const QJsonObject& request);
        QJsonObject setHooks(QLocalSocket* socket, const QJsonObject& request);
        QJsonObject replyHook(QLocalSocket* socket, const QJsonObject& request);
        bool hasSubscriber(const QString& event) const;
        void publishEvent(const QString& event, const QJsonObject& data);
        void publishMeasurement(const QString& cameraId,
                                const QString& moduleId,
                                qint64 frameIndex,
                                const QVariantList& measurements);
        void sendRequestResponse(QLocalSocket* socket,
                                 QJsonObject response,
                                 const QJsonValue& requestId);
        bool processAsyncRequest(const QJsonObject& request, const ResponseCallback& finish);
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
        // The events each subscribed socket receives
        QHash<QLocalSocket*, QSet<QString>> m_subscriptions;
        // The socket that set the hooks and receives their requests
        QLocalSocket* m_hookOwner{nullptr};
        QString m_configPath;
        QJsonObject m_lastPreviewState;
        QThreadPool m_taskPool;
        QString m_frameMappingCameraId;

        void* m_frameMappingHandle{nullptr};
        uchar* m_frameMappingView{nullptr};
        int m_frameShmFd{-1};
    };
} // namespace scopeone::ui
