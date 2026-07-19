#pragma once

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <memory>

#include "scopeone/ScopeOneCore.h"

class QLocalServer;
class QLocalSocket;

namespace scopeone::ui
{
    class ImageSceneModel;
    class PreviewWidget;

    class ScopeOneLocalApiServer : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneLocalApiServer(scopeone::core::ScopeOneCore* core,
                                        PreviewWidget* previewWidget,
                                        ImageSceneModel* sceneModel,
                                        QObject* parent = nullptr);
        ~ScopeOneLocalApiServer() override;

    private:
        void handleNewConnection();
        void handleSocketReadyRead(QLocalSocket* socket);
        void handleSocketDisconnected(QLocalSocket* socket);
        void sendResponse(QLocalSocket* socket, const QJsonObject& response);
        QJsonObject processRequest(const QJsonObject& request);
        scopeone::core::ExperimentDocument createExperimentDocument();
        QJsonObject experimentStatusResponse(const QString& type,
                                             const QString& experimentId) const;
        bool startRecordingPlan(const scopeone::core::ExperimentPlan& plan,
                                QStringList& startedPreviewCameraIds,
                                QString& errorMessage);
        void handleExperimentRecordingStopped(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> runRecording(
            const scopeone::core::ExperimentPlan& plan,
            int timeoutMs,
            QString& errorMessage);
        bool exportFrameToSharedMemory(const scopeone::core::ImageFrame& frame,
                                       scopeone::core::ImageFrame& exportedFrame,
                                       QString& errorMessage);
        bool importFrameFromSharedMemory(const QString& cameraId,
                                         scopeone::core::ImageFrame& frame,
                                         QString& errorMessage);

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        PreviewWidget* m_previewWidget{nullptr};
        ImageSceneModel* m_sceneModel{nullptr};
        QLocalServer* m_server{nullptr};
        QHash<QLocalSocket*, QByteArray> m_readBuffers;
        QHash<QString, std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> m_sessions;
        QHash<QString, scopeone::core::ExperimentDocument> m_experiments;
        QString m_activeExperimentId;
        QStringList m_experimentStartedPreviewCameraIds;
        bool m_experimentCancelRequested{false};
        QString m_frameMappingCameraId;

        void* m_frameMappingHandle{nullptr};
        uchar* m_frameMappingView{nullptr};
        int m_frameShmFd{-1};
    };
} // namespace scopeone::ui
