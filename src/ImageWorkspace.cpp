#include "ImageWorkspace.h"

#include "PreviewWidget.h"
#include "scopeone/ImageSceneModel.h"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QToolBar>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;

        QString defaultTitle(const RecordingSessionData& session, const QString& cameraId)
        {
            const QString baseName = session.capturePlan().baseName.trimmed();
            return baseName.isEmpty() ? cameraId : baseName + QStringLiteral(" - ") + cameraId;
        }

    }

    struct ImageWorkspace::Document
    {
        QString id;
        QString title;
        std::shared_ptr<RecordingSessionData> session;
        QString cameraId;
        int frameIndex{0};
        int frameCount{0};
        int requestedFrameIndex{0};
        quint64 frameRequestId{0};
        scopeone::core::ImageFrame currentFrame;
        QString activeLayerKey;
        QString layerSourceId;
        bool presentationApplied{false};
    };

    ImageWorkspace::ImageWorkspace(scopeone::core::ScopeOneCore* core,
                                   QWidget* windowParent,
                                   QObject* parent)
        : QObject(parent), m_core(core)
    {
        m_viewerHost = new QWidget(windowParent);
        m_viewerLayout = new QVBoxLayout(m_viewerHost);
        m_viewerLayout->setContentsMargins(0, 0, 0, 0);
        m_viewerLayout->setSpacing(0);

        m_viewerToolbar = new QToolBar(tr("Viewer"), m_viewerHost);
        m_viewerToolbar->setMovable(false);
        m_viewerToolbar->setFloatable(false);
        m_viewerToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_viewerLayout->addWidget(m_viewerToolbar);

        setupViewerToolbar();
        const int viewerToolbarHeight = m_viewerToolbar->sizeHint().height();
        m_viewerToolbar->setMinimumHeight(viewerToolbarHeight);
        m_viewerToolbar->setMaximumHeight(viewerToolbarHeight);
        connect(core, &scopeone::core::ScopeOneCore::recordingSessionFrameReady,
                this, [this](quint64 requestId,
                             const std::shared_ptr<RecordingSessionData>&,
                             const QString&,
                             int frameIndex,
                             const scopeone::core::ImageFrame& frame)
                {
                    const QString documentId = m_frameRequests.take(requestId);
                    Document* document = findDocument(documentId);
                    if (!document || document->frameRequestId != requestId)
                    {
                        return;
                    }
                    document->frameRequestId = 0;
                    if (!frame.isValid() && !document->currentFrame.isValid())
                    {
                        closeDocument(documentId);
                        return;
                    }
                    if (frame.isValid())
                    {
                        document->frameIndex = frameIndex;
                        document->currentFrame = frame;
                        scopeone::core::ImageFrame displayFrame(frame);
                        displayFrame.cameraId = document->layerSourceId;
                        m_core->publishStaticFrame(
                            document->layerSourceId, displayFrame, document->title);
                        if (!document->presentationApplied)
                        {
                            const auto& presentation = document->session->experimentDocument();
                            const auto sourceLayer = std::find_if(
                                presentation.layers.cbegin(), presentation.layers.cend(),
                                [&document](const scopeone::core::DocumentLayer& layer)
                                {
                                    return layer.sourceId == document->cameraId
                                           && layer.kind == scopeone::core::DocumentLayerKind::Raw;
                                });
                            if (sourceLayer != presentation.layers.cend())
                            {
                                auto* scene = m_core->imageSceneModel();
                                scene->setLayerOpacityPercent(
                                    document->activeLayerKey,
                                    sourceLayer->display.opacityPercent);
                                scene->setLayerGamma(
                                    document->activeLayerKey,
                                    sourceLayer->display.gamma);
                                scene->setLayerColormap(
                                    document->activeLayerKey,
                                    sourceLayer->display.colormap);
                                scene->setLayerBlending(
                                    document->activeLayerKey,
                                    sourceLayer->display.blending);
                                scene->setLayerDisplayLevels(
                                    document->activeLayerKey,
                                    sourceLayer->display.levelMin,
                                    sourceLayer->display.levelMax,
                                    sourceLayer->display.levelDomainMax);
                                for (const auto& savedMarkup : presentation.markups)
                                {
                                    if (savedMarkup.layerId != sourceLayer->id)
                                    {
                                        continue;
                                    }
                                    const QString markupId =
                                        savedMarkup.type == scopeone::core::DocumentMarkupType::Line
                                            ? scene->createLine(document->activeLayerKey,
                                                                savedMarkup.start.toPoint(),
                                                                savedMarkup.end.toPoint(),
                                                                savedMarkup.label,
                                                                savedMarkup.role)
                                            : scene->createRect(document->activeLayerKey,
                                                               savedMarkup.rect.toRect(),
                                                               savedMarkup.label,
                                                               savedMarkup.role);
                                    scene->setVisible(markupId, savedMarkup.visible);
                                    scene->setSelected(markupId, savedMarkup.selected);
                                }
                            }
                            document->presentationApplied = true;
                        }
                        if (document->id == m_activeDocumentId)
                        {
                            QStringList visibleLayerKeys;
                            const QString sessionId = document->session->capturePlan().experimentId;
                            for (const auto& candidate : m_documents)
                            {
                                if (candidate->session->capturePlan().experimentId == sessionId
                                    && m_core->imageSceneModel()->layerIds().contains(
                                        candidate->activeLayerKey))
                                {
                                    visibleLayerKeys.append(candidate->activeLayerKey);
                                }
                            }
                            m_core->imageSceneModel()->setVisibleLayers(visibleLayerKeys);
                            emit activeFrameChanged();
                            updateLineProfile(*document);
                        }
                        emit documentsChanged();
                    }
                    if (document->frameIndex != document->requestedFrameIndex)
                    {
                        if (!requestFrame(*document, document->requestedFrameIndex))
                        {
                            document->requestedFrameIndex = document->frameIndex;
                        }
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::recordingSessionClosed,
                this, [this](const QString& sessionId)
                {
                    QStringList documentIds;
                    for (const auto& document : m_documents)
                    {
                        if (document->session->capturePlan().experimentId == sessionId)
                        {
                            documentIds.append(document->id);
                        }
                    }
                    for (const QString& documentId : documentIds)
                    {
                        closeDocument(documentId);
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::imageProcessingFinished,
                this, [this](quint64 requestId,
                             const QString&,
                             const scopeone::core::ImageFrame& frame,
                             const QString& errorMessage)
                {
                    const QString sourceId = m_processingRequests.take(requestId);
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    const Document* source = findDocument(sourceId);
                    const QString resultError = !source && errorMessage.isEmpty()
                                                    ? tr("Source image viewer was closed")
                                                    : errorMessage;
                    QString outputId;
                    if (resultError.isEmpty() && frame.isValid())
                    {
                        outputId = openFrame(frame,
                                             tr("Processed %1").arg(source->title));
                    }
                    emit documentProcessingFinished(requestId, outputId, resultError);
                });
        connect(core, &scopeone::core::ScopeOneCore::stackProcessingProgress,
                this, [this](quint64 requestId, qint64 completed, qint64 total)
                {
                    if (m_processingRequests.contains(requestId))
                    {
                        emit documentProcessingProgress(requestId, completed, total);
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::stackProcessingFinished,
                this, [this](quint64 requestId,
                             const std::shared_ptr<RecordingSessionData>& session,
                             const QString& errorMessage)
                {
                    const QString sourceId = m_processingRequests.take(requestId);
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    const QString resultError = !findDocument(sourceId) && errorMessage.isEmpty()
                                                    ? tr("Source image viewer was closed")
                                                    : errorMessage;
                    QString outputId;
                    if (resultError.isEmpty() && session)
                    {
                        const QStringList ids = openSession(session, tr("Processed Stack"));
                        outputId = ids.value(0);
                        emit sessionAvailable(session, tr("Processed Stack"));
                    }
                    emit documentProcessingFinished(requestId, outputId, resultError);
                });
        connect(core, &scopeone::core::ScopeOneCore::recordingSessionCameraSaveFinished,
                this, [this](const std::shared_ptr<RecordingSessionData>& session,
                             const QString& cameraId,
                             bool success,
                             const QString& message)
                {
                    const QString key = session
                                            ? session->capturePlan().experimentId
                                                  + QLatin1Char('\n') + cameraId
                                            : QString{};
                    const QString documentId = m_saveRequests.take(key);
                    if (!documentId.isEmpty())
                    {
                        emit documentSaveFinished(documentId, success, message);
                    }
                });
    }

    ImageWorkspace::~ImageWorkspace() = default;

    QWidget* ImageWorkspace::viewerHost() const
    {
        return m_viewerHost;
    }

    // Builds the compact controls used by the active image viewer
    void ImageWorkspace::setupViewerToolbar()
    {
        m_fitToWindowAction = m_viewerToolbar->addAction(tr("Fit"));
        m_fitToWindowAction->setCheckable(true);
        m_fitToWindowAction->setToolTip(tr("Fit the image to the viewer"));
        connect(m_fitToWindowAction, &QAction::toggled, this,
                [this](bool enabled)
                {
                    if (PreviewWidget* preview = activePreviewWidget())
                    {
                        preview->setFitToWindow(enabled);
                    }
                    updateViewerToolbar();
                });

        m_oneToOneAction = m_viewerToolbar->addAction(tr("1:1"));
        m_oneToOneAction->setToolTip(tr("Show the image at native pixel size"));
        connect(m_oneToOneAction, &QAction::triggered, this,
                [this]()
                {
                    if (PreviewWidget* preview = activePreviewWidget())
                    {
                        preview->setFitToWindow(false);
                        preview->setZoomPercent(100);
                    }
                });

        m_zoomCombo = new QComboBox(m_viewerToolbar);
        m_zoomCombo->setEditable(true);
        m_zoomCombo->setMinimumWidth(76);
        m_zoomCombo->setInsertPolicy(QComboBox::NoInsert);
        m_zoomCombo->addItem(tr("Fit"));
        m_zoomCombo->addItems({QStringLiteral("25%"),
                               QStringLiteral("50%"),
                               QStringLiteral("75%"),
                               QStringLiteral("100%"),
                               QStringLiteral("150%"),
                               QStringLiteral("200%"),
                               QStringLiteral("300%"),
                               QStringLiteral("400%"),
                               QStringLiteral("800%")});
        m_zoomCombo->setToolTip(tr("Choose the viewport zoom percentage"));
        m_viewerToolbar->addWidget(m_zoomCombo);
        auto applyZoomText = [this]()
        {
            PreviewWidget* preview = activePreviewWidget();
            if (!preview)
            {
                return;
            }
            const QString text = m_zoomCombo->currentText().trimmed();
            if (text.compare(QStringLiteral("Fit"), Qt::CaseInsensitive) == 0)
            {
                preview->setFitToWindow(true);
                return;
            }

            QString numericText = text;
            if (numericText.endsWith(QLatin1Char('%')))
            {
                numericText.chop(1);
            }
            bool ok = false;
            const int percent = numericText.toInt(&ok);
            if (ok)
            {
                preview->setFitToWindow(false);
                preview->setZoomPercent(percent);
            }
            updateViewerToolbar();
        };
        connect(m_zoomCombo, qOverload<int>(&QComboBox::activated), this,
                [applyZoomText](int) { applyZoomText(); });
        connect(m_zoomCombo->lineEdit(), &QLineEdit::editingFinished,
                this, applyZoomText);

        m_viewerToolbar->addSeparator();
        m_layoutCombo = new QComboBox(m_viewerToolbar);
        m_layoutCombo->addItem(tr("Grid View (G)"));
        m_layoutCombo->addItem(tr("Overlay (G)"));
        m_layoutCombo->setToolTip(tr("Choose how multiple image layers are arranged"));
        m_viewerToolbar->addWidget(m_layoutCombo);
        connect(m_layoutCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index)
                {
                    if (PreviewWidget* preview = activePreviewWidget())
                    {
                        preview->setLayerLayoutMode(
                            index == 1 ? PreviewWidget::LayerLayoutMode::Overlay
                                       : PreviewWidget::LayerLayoutMode::SideBySide);
                    }
                });

        m_viewerToolbar->addSeparator();
        m_dimensionAction = m_viewerToolbar->addAction(tr("3D Surface"));
        m_dimensionAction->setCheckable(true);
        m_dimensionAction->setToolTip(tr("Switch between flat 2D and 3D surface view"));
        connect(m_dimensionAction, &QAction::toggled, this,
                [this](bool enabled)
                {
                    if (PreviewWidget* preview = activePreviewWidget())
                    {
                        preview->setViewDimensionMode(
                            enabled ? PreviewWidget::ViewDimensionMode::ThreeDimensional
                                    : PreviewWidget::ViewDimensionMode::TwoDimensional);
                    }
                    updateViewerToolbar();
                });

        m_reset3dAction = m_viewerToolbar->addAction(tr("Reset 3D"));
        m_reset3dAction->setToolTip(tr("Reset the 3D camera view"));
        connect(m_reset3dAction, &QAction::triggered, this,
                [this]()
                {
                    if (PreviewWidget* preview = activePreviewWidget())
                    {
                        preview->reset3dCamera();
                    }
                });

    }

    // Refreshes viewer controls when the active document or display state changes
    void ImageWorkspace::updateViewerToolbar()
    {
        PreviewWidget* preview = activePreviewWidget();
        m_fitToWindowAction->setEnabled(preview != nullptr);
        m_oneToOneAction->setEnabled(preview != nullptr);
        m_zoomCombo->setEnabled(preview != nullptr);
        m_dimensionAction->setEnabled(preview != nullptr);
        m_reset3dAction->setEnabled(
            preview && preview->viewDimensionMode() == PreviewWidget::ViewDimensionMode::ThreeDimensional);
        if (preview)
        {
            {
                const QSignalBlocker blocker(m_fitToWindowAction);
                m_fitToWindowAction->setChecked(preview->isFitToWindow());
            }
            {
                const QSignalBlocker blocker(m_zoomCombo);
                m_zoomCombo->setEditText(
                    preview->isFitToWindow()
                        ? tr("Fit")
                        : QStringLiteral("%1%").arg(preview->zoomPercent()));
            }
            {
                const QSignalBlocker blocker(m_layoutCombo);
                m_layoutCombo->setCurrentIndex(
                    preview->layerLayoutMode() == PreviewWidget::LayerLayoutMode::Overlay ? 1 : 0);
            }
            {
                const QSignalBlocker blocker(m_dimensionAction);
                m_dimensionAction->setChecked(
                    preview->viewDimensionMode() == PreviewWidget::ViewDimensionMode::ThreeDimensional);
            }
        }

    }

    void ImageWorkspace::setLiveViewer(PreviewWidget* previewWidget)
    {
        m_livePreviewWidget = previewWidget;
        m_viewerLayout->addWidget(previewWidget, 1);
        connectViewer(previewWidget);
        m_liveLayerKey = previewWidget->visibleLayerKeys().value(0);
        updateViewerToolbar();
    }

    void ImageWorkspace::activateLiveViewer()
    {
        if (!m_livePreviewWidget)
        {
            return;
        }
        if (m_activeDocumentId.isEmpty())
        {
            return;
        }
        m_activeDocumentId.clear();
        emit activeDocumentChanged({});
        emit activeViewerChanged();
        emit activeLayerChanged(activeLayerKey());
        emit documentsChanged();
        updateViewerToolbar();
    }

    void ImageWorkspace::setVisibleLayers(const QStringList& layerKeys, bool sideBySide)
    {
        Document* document = nullptr;
        for (const QString& layerKey : layerKeys)
        {
            document = findDocumentByLayerKey(layerKey);
            if (document)
            {
                break;
            }
        }
        if (document)
        {
            setActiveDocument(document->id);
        }
        else
        {
            activateLiveViewer();
        }

        auto* scene = m_core->imageSceneModel();
        auto* preview = m_livePreviewWidget;
        if (!scene || !preview)
        {
            return;
        }
        scene->setVisibleLayers(layerKeys);
        syncActiveLayer();
        preview->setLayerLayoutMode(sideBySide
                                        ? PreviewWidget::LayerLayoutMode::SideBySide
                                        : PreviewWidget::LayerLayoutMode::Overlay);
    }

    void ImageWorkspace::setLayerSliceIndex(const QString& layerKey, int sliceIndex)
    {
        const QString sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
        Document* document = findDocumentByLayerKey(layerKey);
        if (!document || document->layerSourceId != sourceId)
        {
            m_core->setLayerSliceIndex(layerKey, sliceIndex);
            return;
        }
        requestFrame(*document, sliceIndex);
    }

    int ImageWorkspace::layerSliceCount(const QString& layerKey) const
    {
        const Document* document = findDocumentByLayerKey(layerKey);
        return document ? document->frameCount : m_core->layerSliceCount(layerKey);
    }

    QStringList ImageWorkspace::openSession(const std::shared_ptr<RecordingSessionData>& session,
                                            const QString& title,
                                            const QString& cameraId)
    {
        if (!session)
        {
            return {};
        }
        const QStringList cameras = cameraId.trimmed().isEmpty()
                                        ? session->recordedCameraIds()
                                        : QStringList{cameraId.trimmed()};
        QStringList openedIds;
        for (const QString& camera : cameras)
        {
            const qint64 count = session->recordedFrameCount(camera);
            if (count <= 0 || count > (std::numeric_limits<int>::max)())
            {
                continue;
            }
            const QString sessionId = session->capturePlan().experimentId;
            auto existing = std::find_if(m_documents.begin(), m_documents.end(),
                                         [&sessionId, &camera](const auto& document)
                                         {
                                             return document->session->capturePlan().experimentId == sessionId
                                                    && document->cameraId == camera;
                                         });
            if (existing != m_documents.end())
            {
                activateDocument((*existing)->id);
                openedIds.append((*existing)->id);
                continue;
            }

            auto document = std::make_unique<Document>();
            document->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            document->title = title.trimmed().isEmpty() ? defaultTitle(*session, camera)
                                                        : title.trimmed();
            if (cameras.size() > 1 && !title.trimmed().isEmpty())
            {
                document->title += QStringLiteral(" - ") + camera;
            }
            document->session = session;
            document->cameraId = camera;
            document->frameCount = static_cast<int>(count);
            document->layerSourceId = QStringLiteral("gallery:%1_%2_%3")
                                          .arg(sessionId, camera, document->id);
            document->activeLayerKey = scopeone::core::ScopeOneCore::staticLayerKey(
                document->layerSourceId);
            const QString id = document->id;
            m_documents.push_back(std::move(document));
            if (!requestFrame(*m_documents.back(), 0))
            {
                closeDocument(id);
                continue;
            }
            openedIds.append(id);
        }
        if (!openedIds.isEmpty())
        {
            QStringList layerKeys;
            for (const QString& id : openedIds)
            {
                layerKeys.append(findDocument(id)->activeLayerKey);
            }
            m_core->imageSceneModel()->setVisibleLayers(
                [this, &layerKeys]
                {
                    QStringList existing;
                    for (const QString& layerKey : layerKeys)
                    {
                        if (m_core->imageSceneModel()->layerIds().contains(layerKey))
                        {
                            existing.append(layerKey);
                        }
                    }
                    return existing;
                }());
            m_livePreviewWidget->setLayerLayoutMode(
                layerKeys.size() > 1
                    ? PreviewWidget::LayerLayoutMode::SideBySide
                    : PreviewWidget::LayerLayoutMode::Overlay);
            setActiveDocument(openedIds.constLast());
        }
        return openedIds;
    }

    QString ImageWorkspace::openFrame(const scopeone::core::ImageFrame& frame,
                                      const QString& title)
    {
        if (!frame.isValid())
        {
            return {};
        }
        scopeone::core::ExperimentPlan plan;
        plan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.cameraIds = {frame.cameraId};
        plan.streamToDisk = false;
        plan.baseName = title.trimmed().isEmpty()
                            ? QStringLiteral("image_%1").arg(
                                  QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz")))
                            : title.trimmed();
        const auto session = m_core->createFrameSession({frame}, plan);
        if (!session)
        {
            return {};
        }
        emit sessionAvailable(session, plan.baseName);
        return openSession(session, plan.baseName, frame.cameraId).value(0);
    }

    QList<ImageDocumentInfo> ImageWorkspace::documents() const
    {
        QList<ImageDocumentInfo> result;
        result.reserve(static_cast<qsizetype>(m_documents.size()));
        for (const auto& document : m_documents)
        {
            result.append({document->id,
                           document->title,
                           document->session->capturePlan().experimentId,
                           document->cameraId,
                           document->frameIndex,
                           document->frameCount,
                           document->currentFrame.isValid(),
                           document->id == m_activeDocumentId});
        }
        return result;
    }

    ImageDocumentInfo ImageWorkspace::document(const QString& documentId) const
    {
        const Document* document = findDocument(documentId);
        return document
                   ? ImageDocumentInfo{document->id,
                                       document->title,
                                       document->session->capturePlan().experimentId,
                                       document->cameraId,
                                       document->frameIndex,
                                       document->frameCount,
                                       document->currentFrame.isValid(),
                                       document->id == m_activeDocumentId}
                   : ImageDocumentInfo{};
    }

    QString ImageWorkspace::activeDocumentId() const
    {
        return m_activeDocumentId;
    }

    bool ImageWorkspace::isLiveViewerActive() const
    {
        return m_activeDocumentId.isEmpty();
    }

    scopeone::core::ImageSceneModel* ImageWorkspace::activeSceneModel() const
    {
        return sceneModel(m_activeDocumentId);
    }

    PreviewWidget* ImageWorkspace::activePreviewWidget() const
    {
        return previewWidget(m_activeDocumentId);
    }

    scopeone::core::ImageSceneModel* ImageWorkspace::sceneModel(const QString& documentId) const
    {
        Q_UNUSED(documentId);
        return m_core->imageSceneModel();
    }

    PreviewWidget* ImageWorkspace::previewWidget(const QString& documentId) const
    {
        Q_UNUSED(documentId);
        return m_livePreviewWidget;
    }

    scopeone::core::ImageFrame ImageWorkspace::frameForLayer(const QString& layerKey) const
    {
        return m_core->graphFrame(layerKey);
    }

    bool ImageWorkspace::histogram(
        const QString& layerKey,
        scopeone::core::ScopeOneCore::HistogramStats& stats) const
    {
        return m_core->getLayerHistogram(layerKey, stats);
    }

    void ImageWorkspace::requestHistogram(const QString& layerKey)
    {
        m_core->setActiveHistogramLayer(layerKey);
    }

    bool ImageWorkspace::autoLayerLevels(const QString& layerKey)
    {
        return m_core->autoLayerLevels(layerKey);
    }

    bool ImageWorkspace::fullLayerLevels(const QString& layerKey)
    {
        return m_core->fullLayerLevels(layerKey);
    }

    bool ImageWorkspace::setLayerAutoStretchEnabled(const QString& layerKey, bool enabled)
    {
        return m_core->setLayerAutoStretchEnabled(layerKey, enabled);
    }

    bool ImageWorkspace::layerAutoStretchEnabled(const QString& layerKey) const
    {
        scopeone::core::ImageSceneModel* scene = activeSceneModel();
        return scene && scene->layerAutoStretchEnabled(layerKey);
    }

    bool ImageWorkspace::lineProfile(const QString& layerKey,
                                     const QPoint& start,
                                     const QPoint& end,
                                     QVector<int>& values) const
    {
        return m_core->getLineProfile(layerKey, start, end, values);
    }

    bool ImageWorkspace::pixelValue(const QString& layerKey, const QPoint& point, int& value) const
    {
        return m_core->graphPixelValue(layerKey, point, value);
    }

    double ImageWorkspace::pixelSizeUm(const QString& layerKey) const
    {
        const Document* document = findDocumentByLayerKey(layerKey);
        if (!document)
        {
            return m_core->cameraPixelSizeUm(
                scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey));
        }
        return document->session->cameraPixelSizeUm(document->cameraId);
    }

    QString ImageWorkspace::activeLayerKey() const
    {
        const auto* scene = m_core->imageSceneModel();
        if (!scene)
        {
            return {};
        }
        if (scene->visibleLayerIds().contains(m_liveLayerKey))
        {
            return m_liveLayerKey;
        }
        return scene->visibleLayerIds().value(0);
    }

    void ImageWorkspace::setActiveLayerKey(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        const QString previousLayerKey = activeLayerKey();
        Document* document = findDocumentByLayerKey(normalizedLayerKey);
        if (document)
        {
            setActiveDocument(document->id);
            m_liveLayerKey = normalizedLayerKey;
        }
        else if (m_core->imageSceneModel()->layerIds().contains(normalizedLayerKey))
        {
            activateLiveViewer();
            m_liveLayerKey = normalizedLayerKey;
        }
        if (PreviewWidget* preview = activePreviewWidget())
        {
            preview->setActiveLayerKey(normalizedLayerKey);
        }
        if (previousLayerKey != normalizedLayerKey)
        {
            emit activeLayerChanged(normalizedLayerKey);
        }
        updateViewerToolbar();
    }

    // Keeps the active layer aligned with the visible layers in one viewer
    void ImageWorkspace::syncActiveLayer()
    {
        scopeone::core::ImageSceneModel* scene = m_core->imageSceneModel();
        const QString previousLayerKey = activeLayerKey();
        const QStringList visibleLayerKeys = scene->visibleLayerIds();
        if (!visibleLayerKeys.contains(m_liveLayerKey))
        {
            m_liveLayerKey = visibleLayerKeys.value(0);
        }

        updateViewerToolbar();
        if (previousLayerKey != activeLayerKey())
        {
            emit activeLayerChanged(activeLayerKey());
        }
    }

    scopeone::core::ImageFrame ImageWorkspace::currentFrame(const QString& documentId) const
    {
        const Document* document = findDocument(documentId);
        return document ? document->currentFrame : scopeone::core::ImageFrame{};
    }

    bool ImageWorkspace::activateDocument(const QString& documentId)
    {
        Document* document = findDocument(documentId);
        if (!document)
        {
            return false;
        }
        setActiveDocument(documentId);
        m_core->imageSceneModel()->setVisibleLayers({document->activeLayerKey});
        return true;
    }

    void ImageWorkspace::setActiveDocument(const QString& documentId)
    {
        if (m_activeDocumentId == documentId || !findDocument(documentId))
        {
            return;
        }
        m_activeDocumentId = documentId;
        emit activeDocumentChanged(documentId);
        emit activeViewerChanged();
        emit activeLayerChanged(activeLayerKey());
        emit documentsChanged();
        updateViewerToolbar();
    }

    bool ImageWorkspace::closeDocument(const QString& documentId)
    {
        Document* document = findDocument(documentId);
        if (!document)
        {
            return false;
        }
        removeDocument(documentId);
        return true;
    }

    quint64 ImageWorkspace::processDocument(const QString& documentId, bool completeStack)
    {
        Document* document = findDocument(documentId);
        if (!document)
        {
            return 0;
        }
        const quint64 requestId = completeStack
                                      ? m_core->requestRecordingSessionStackProcessing(
                                            document->session->capturePlan().experimentId,
                                            document->cameraId)
                                      : m_core->requestImageProcessing(document->currentFrame,
                                                                       document->id);
        if (requestId != 0)
        {
            m_processingRequests.insert(requestId, document->id);
        }
        return requestId;
    }

    bool ImageWorkspace::saveDocument(
        const QString& documentId,
        const scopeone::core::ScopeOneCore::RecordingSaveOptions& options)
    {
        Document* document = findDocument(documentId);
        if (!document
            || !m_core->saveRecordingSessionCamera(
                document->session,
                document->cameraId,
                options,
                &m_core->imageSceneModel()->document()))
        {
            return false;
        }
        m_saveRequests.insert(document->session->capturePlan().experimentId
                                  + QLatin1Char('\n') + document->cameraId,
                              document->id);
        return true;
    }

    void ImageWorkspace::saveDocumentAs(const QString& documentId)
    {
        Document* document = findDocument(documentId);
        if (!document)
        {
            return;
        }
        const QString saveDir = QFileDialog::getExistingDirectory(
            m_viewerHost,
            tr("Select Dataset Folder"),
            QDir::homePath());
        if (saveDir.isEmpty())
        {
            return;
        }
        bool accepted = false;
        QString baseName = QInputDialog::getText(
                               m_viewerHost,
                               tr("Save Image Dataset As"),
                               tr("Dataset name and optional format suffix"),
                               QLineEdit::Normal,
                               document->title + QStringLiteral(".ome.tiff"),
                               &accepted)
                               .trimmed();
        if (!accepted || baseName.isEmpty())
        {
            return;
        }
        scopeone::core::ScopeOneCore::RecordingSaveOptions options;
        if (baseName.endsWith(QStringLiteral(".ome.tiff"), Qt::CaseInsensitive))
        {
            baseName.chop(9);
            options.format = scopeone::core::RecordingFormat::OmeTiff;
        }
        else if (baseName.endsWith(QStringLiteral(".ome.zarr"), Qt::CaseInsensitive))
        {
            baseName.chop(9);
            options.format = scopeone::core::RecordingFormat::OmeZarr;
        }
        else if (baseName.endsWith(QStringLiteral(".tiff"), Qt::CaseInsensitive))
        {
            baseName.chop(5);
            options.format = scopeone::core::RecordingFormat::Tiff;
        }
        else if (baseName.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive))
        {
            baseName.chop(4);
            options.format = scopeone::core::RecordingFormat::Binary;
        }
        options.saveDir = saveDir;
        options.baseName = baseName;
        options.enableCompression = options.format != scopeone::core::RecordingFormat::Binary;
        saveDocument(document->id, options);
    }

    ImageWorkspace::Document* ImageWorkspace::findDocument(const QString& documentId) const
    {
        const QString id = documentId.trimmed().isEmpty() ? m_activeDocumentId
                                                          : documentId.trimmed();
        const auto it = std::find_if(m_documents.begin(), m_documents.end(),
                                     [&id](const auto& document)
                                     {
                                         return document->id == id;
                                     });
        return it == m_documents.end() ? nullptr : it->get();
    }

    ImageWorkspace::Document* ImageWorkspace::findDocumentByLayerKey(const QString& layerKey) const
    {
        const auto it = std::find_if(m_documents.cbegin(), m_documents.cend(),
                                     [&layerKey](const auto& document)
                                     {
                                         return document->activeLayerKey == layerKey;
                                     });
        return it == m_documents.cend() ? nullptr : it->get();
    }

    bool ImageWorkspace::requestFrame(Document& document, int frameIndex)
    {
        document.requestedFrameIndex = qBound(0, frameIndex, document.frameCount - 1);
        if (document.frameRequestId != 0)
        {
            return true;
        }
        document.frameRequestId = m_core->requestRecordingSessionFrame(
            document.session, document.cameraId, document.requestedFrameIndex);
        if (document.frameRequestId != 0)
        {
            m_frameRequests.insert(document.frameRequestId, document.id);
            return true;
        }
        return false;
    }

    void ImageWorkspace::removeDocument(const QString& documentId)
    {
        const auto it = std::find_if(m_documents.begin(), m_documents.end(),
                                     [&documentId](const auto& document)
                                     {
                                         return document->id == documentId;
                                     });
        if (it == m_documents.end())
        {
            return;
        }
        if ((*it)->frameRequestId != 0)
        {
            m_frameRequests.remove((*it)->frameRequestId);
        }
        for (auto request = m_processingRequests.constBegin();
             request != m_processingRequests.constEnd(); ++request)
        {
            if (request.value() == documentId)
            {
                m_core->cancelProcessingRequest(request.key());
            }
        }
        m_core->removeStaticFrame((*it)->layerSourceId);
        m_documents.erase(it);
        if (m_activeDocumentId == documentId)
        {
            m_activeDocumentId.clear();
            emit activeDocumentChanged(m_activeDocumentId);
            emit activeViewerChanged();
            emit activeLayerChanged(activeLayerKey());
            updateViewerToolbar();
        }
        emit documentsChanged();
    }

    void ImageWorkspace::connectViewer(PreviewWidget* preview)
    {
        connect(preview, &PreviewWidget::measurementLineDrawn,
                this, &ImageWorkspace::measurementLineDrawn);
        connect(preview, &PreviewWidget::measurementLineInspected,
                this, &ImageWorkspace::measurementLineInspected);
        connect(preview, &PreviewWidget::measurementLineCleared,
                this, &ImageWorkspace::measurementLineCleared);
        connect(preview, &PreviewWidget::layerClicked,
                this, [this](const QString& layerKey) { setActiveLayerKey(layerKey); });
        connect(preview, &PreviewWidget::mousePositionChanged,
                this, [this](const QPoint& position)
                {
                    emit mousePositionChanged(position);
                });
        auto* scene = preview->sceneModel();
        connect(preview, &PreviewWidget::availableLayerKeysChanged,
                this, [this](const QStringList&) { updateViewerToolbar(); });
        connect(preview, &PreviewWidget::zoomLevelChanged,
                this, [this](int) { updateViewerToolbar(); });
        connect(preview, &PreviewWidget::fitToWindowChanged,
                this, [this](bool) { updateViewerToolbar(); });
        connect(preview, &PreviewWidget::layerLayoutModeChanged,
                this, [this](PreviewWidget::LayerLayoutMode) { updateViewerToolbar(); });
        connect(preview, &PreviewWidget::viewDimensionModeChanged,
                this, [this](PreviewWidget::ViewDimensionMode mode)
                {
                    Q_UNUSED(mode);
                    emit viewDimensionModeChanged();
                    updateViewerToolbar();
                });
        connect(scene, &scopeone::core::ImageSceneModel::layersChanged,
                this, &ImageWorkspace::syncActiveLayer);
        connect(scene, &scopeone::core::ImageSceneModel::markupsChanged,
                this, [this]()
                {
                    Document* document = findDocument(m_activeDocumentId);
                    if (document)
                    {
                        updateLineProfile(*document);
                    }
                });
    }

    void ImageWorkspace::updateLineProfile(Document& document)
    {
        for (const auto& markup : m_core->imageSceneModel()->markups())
        {
            if (markup.layerKey == activeLayerKey()
                && markup.role == scopeone::core::ImageSceneModel::MarkupRole::CrossSection)
            {
                QVector<int> values;
                if (lineProfile(markup.layerKey, markup.start, markup.end, values))
                {
                    emit lineProfileUpdated(markup.layerKey, values);
                }
                return;
            }
        }
    }
}
