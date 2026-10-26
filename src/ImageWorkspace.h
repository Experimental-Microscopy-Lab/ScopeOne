#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <memory>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QWidget;
class QLabel;
class QToolBar;
class QToolButton;
class QVBoxLayout;

namespace scopeone::core
{
    class ImageSceneModel;
}

namespace scopeone::ui
{
    class PreviewWidget;

    struct ImageDocumentInfo
    {
        QString id;
        QString title;
        QString sessionId;
        QString cameraId;
        int frameIndex{0};
        int frameCount{0};
        bool ready{false};
        bool active{false};

        bool isValid() const { return !id.isEmpty(); }
    };

    class ImageWorkspace : public QObject
    {
        Q_OBJECT

    public:
        explicit ImageWorkspace(scopeone::core::ScopeOneCore* core,
                                QWidget* windowParent,
                                QObject* parent = nullptr);
        ~ImageWorkspace() override;

        QWidget* viewerHost() const;
        // Moves the selection tools and the view controls into the toolbar of the window
        void moveViewerControlsTo(QToolBar* target);
        void setLiveViewer(PreviewWidget* previewWidget);
        void activateLiveViewer();
        void setVisibleLayers(const QStringList& layerKeys, bool sideBySide = false);
        void setLayerSliceIndex(const QString& layerKey, int sliceIndex);
        int layerSliceCount(const QString& layerKey) const;
        // How the slices of the image of a layer are arranged along the channel, Z and time axes
        scopeone::core::StackShape layerStackShape(const QString& layerKey) const;
        void refreshStackShape(const QString& layerKey) { emit stackShapeChanged(layerKey); }
        // Whether the channels of the image of a layer are shown on top of each other
        bool isComposite(const QString& layerKey) const;
        void setComposite(const QString& layerKey, bool enabled);

        QStringList openSession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& title = QString(),
            const QString& cameraId = QString());
        QString openFrame(const scopeone::core::ImageFrame& frame,
                          const QString& title);
        // The session behind an image document, which holds all of its frames
        std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> documentSession(
            const QString& documentId = QString()) const;
        QList<ImageDocumentInfo> documents() const;
        ImageDocumentInfo document(const QString& documentId = QString()) const;
        QString activeDocumentId() const;
        bool isLiveViewerActive() const;
        scopeone::core::ImageSceneModel* activeSceneModel() const;
        PreviewWidget* activePreviewWidget() const;
        scopeone::core::ImageFrame frameForLayer(const QString& layerKey) const;
        bool histogram(const QString& layerKey,
                       scopeone::core::ScopeOneCore::HistogramStats& stats) const;
        void requestHistogram(const QString& layerKey);
        bool autoLayerLevels(const QString& layerKey);
        bool fullLayerLevels(const QString& layerKey);
        bool setLayerAutoStretchEnabled(const QString& layerKey, bool enabled);
        bool layerAutoStretchEnabled(const QString& layerKey) const;
        bool lineProfile(const QString& layerKey,
                         const QPoint& start,
                         const QPoint& end,
                         QVector<int>& values) const;
        bool pixelValue(const QString& layerKey, const QPoint& point, int& value) const;
        double pixelSizeUm(const QString& layerKey) const;
        QString activeLayerKey() const;
        void setActiveLayerKey(const QString& layerKey);
        scopeone::core::ImageSceneModel* sceneModel(const QString& documentId) const;
        PreviewWidget* previewWidget(const QString& documentId) const;
        scopeone::core::ImageFrame currentFrame(const QString& documentId = QString()) const;
        bool activateDocument(const QString& documentId);
        bool closeDocument(const QString& documentId);
        quint64 processDocument(const QString& documentId, bool completeStack);
        bool saveDocument(const QString& documentId,
                          const scopeone::core::ScopeOneCore::RecordingSaveOptions& options);
        void saveDocumentAs(const QString& documentId = QString());

    signals:
        void documentsChanged();
        // The axes of the slices of a layer were changed by the user
        void stackShapeChanged(const QString& layerKey);
        void activeDocumentChanged(const QString& documentId);
        void activeViewerChanged();
        void viewDimensionModeChanged();
        void activeLayerChanged(const QString& layerKey);
        void activeFrameChanged();
        void mousePositionChanged(const QPoint& widgetPos);
        void documentProcessingProgress(quint64 requestId, qint64 completed, qint64 total);
        void documentProcessingFinished(quint64 requestId,
                                        const QString& outputDocumentId,
                                        const QString& errorMessage);
        void documentSaveFinished(const QString& documentId,
                                  bool success,
                                  const QString& message);
        void sessionAvailable(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& title);
        void lineProfileUpdated(const QString& layerKey, const QVector<int>& values);

    private:
        struct Document;
        Document* findDocument(const QString& documentId) const;
        Document* findDocumentByLayerKey(const QString& layerKey) const;
        bool requestFrame(Document& document, int frameIndex);
        void removeDocument(const QString& documentId);
        void connectViewer(PreviewWidget* previewWidget);
        void setupViewerToolbar();
        void setupToolStrip();
        void updateViewerToolbar();
        void setActiveDocument(const QString& documentId);
        void publishChannels(Document& document);
        void syncActiveLayer();
        void updateLineProfile(Document& document);

        scopeone::core::ScopeOneCore* m_core{nullptr};
        QWidget* m_viewerHost{nullptr};
        QVBoxLayout* m_viewerLayout{nullptr};
        QToolBar* m_viewerToolbar{nullptr};
        QWidget* m_toolStrip{nullptr};
        QList<QPair<QToolButton*, int>> m_toolStripButtons;
        QAction* m_fitToWindowAction{nullptr};
        QAction* m_oneToOneAction{nullptr};
        QAction* m_dimensionAction{nullptr};
        QAction* m_reset3dAction{nullptr};
        QComboBox* m_layoutCombo{nullptr};
        QComboBox* m_zoomCombo{nullptr};
        PreviewWidget* m_livePreviewWidget{nullptr};
        std::vector<std::unique_ptr<Document>> m_documents;
        QString m_activeDocumentId;
        QString m_liveLayerKey;
        QHash<quint64, QString> m_frameRequests;
        QHash<quint64, QString> m_processingRequests;
        QHash<QString, QString> m_saveRequests;
    };
}
