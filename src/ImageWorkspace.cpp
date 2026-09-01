#include "ImageWorkspace.h"

#include "PreviewWidget.h"
#include "scopeone/ImageSceneModel.h"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSlider>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <limits>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;

        class ImageDocumentPage final : public QWidget
        {
            Q_OBJECT

        public:
            ImageDocumentPage(const QString& documentId,
                              const QString& title,
                              const QString& cameraId,
                              const std::shared_ptr<RecordingSessionData>& session,
                              const scopeone::core::ExperimentDocument& presentation,
                              int frameCount,
                              QWidget* parent)
                : QWidget(parent), m_documentId(documentId), m_sourceId(cameraId)
            {
                auto* scene = new scopeone::core::ImageSceneModel(this);
                scopeone::core::DocumentLayer layer;
                auto sourceLayer = std::find_if(
                    presentation.layers.cbegin(), presentation.layers.cend(),
                    [&cameraId](const scopeone::core::DocumentLayer& candidate)
                    {
                        return candidate.sourceId == cameraId
                               && candidate.kind == scopeone::core::DocumentLayerKind::Raw;
                    });
                if (sourceLayer == presentation.layers.cend())
                {
                    sourceLayer = std::find_if(
                        presentation.layers.cbegin(), presentation.layers.cend(),
                        [&cameraId](const scopeone::core::DocumentLayer& candidate)
                        {
                            return candidate.sourceId == cameraId;
                        });
                }
                if (sourceLayer != presentation.layers.cend())
                {
                    layer = *sourceLayer;
                }
                layer.id = scopeone::core::ScopeOneCore::staticLayerKey(cameraId);
                layer.sourceId = cameraId;
                layer.name = title;
                layer.kind = scopeone::core::DocumentLayerKind::Gallery;
                layer.display.visible = true;
                scene->ensureLayer(layer);
                scene->setVisibleLayers({layer.id});
                if (sourceLayer != presentation.layers.cend())
                {
                    for (const auto& savedMarkup : presentation.markups)
                    {
                        if (savedMarkup.layerId != sourceLayer->id)
                        {
                            continue;
                        }
                        const QString markupId =
                            savedMarkup.type == scopeone::core::DocumentMarkupType::Line
                                ? scene->createLine(layer.id,
                                                    savedMarkup.start.toPoint(),
                                                    savedMarkup.end.toPoint(),
                                                    savedMarkup.label,
                                                    savedMarkup.role)
                                : scene->createRect(layer.id,
                                                   savedMarkup.rect.toRect(),
                                                   savedMarkup.label,
                                                   savedMarkup.role);
                        scene->setVisible(markupId, savedMarkup.visible);
                        scene->setSelected(markupId, savedMarkup.selected);
                    }
                }

                auto* layout = new QVBoxLayout(this);
                layout->setContentsMargins(0, 0, 0, 0);
                m_preview = new PreviewWidget(scene, this);
                m_preview->setPixelSizeCallback([session, cameraId](const QString&)
                {
                    return session ? session->cameraPixelSizeUm(cameraId) : 0.0;
                });
                layout->addWidget(m_preview, 1);

                auto* navigation = new QWidget(this);
                auto* navigationLayout = new QHBoxLayout(navigation);
                navigationLayout->setContentsMargins(8, 4, 8, 4);
                m_slider = new QSlider(Qt::Horizontal, navigation);
                m_slider->setRange(0, qMax(0, frameCount - 1));
                m_frameLabel = new QLabel(navigation);
                m_frameLabel->setMinimumWidth(90);
                m_frameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                navigationLayout->addWidget(m_slider, 1);
                navigationLayout->addWidget(m_frameLabel);
                navigation->setVisible(frameCount > 1);
                layout->addWidget(navigation);
                updateFrameLabel(0, frameCount);

                connect(m_slider, &QSlider::valueChanged, this,
                        [this, frameCount](int index)
                        {
                            updateFrameLabel(index, frameCount);
                            emit frameIndexRequested(m_documentId, index);
                        });
            }

            void showFrame(const scopeone::core::ImageFrame& frame)
            {
                scopeone::core::ImageFrame displayFrame(frame);
                displayFrame.cameraId = m_sourceId;
                sceneModel()->updateLayerFrame(
                    scopeone::core::ScopeOneCore::staticLayerKey(m_sourceId), displayFrame);
                m_preview->setGraphStaticLayerFrame(m_sourceId, displayFrame);
            }

            scopeone::core::ImageSceneModel* sceneModel() const
            {
                return m_preview->sceneModel();
            }

            PreviewWidget* previewWidget() const
            {
                return m_preview;
            }

            void setFrameIndex(int index)
            {
                const QSignalBlocker blocker(m_slider);
                m_slider->setValue(index);
                updateFrameLabel(index, m_slider->maximum() + 1);
            }

        signals:
            void frameIndexRequested(const QString& documentId, int frameIndex);

        private:
            void updateFrameLabel(int index, int count)
            {
                m_frameLabel->setText(QStringLiteral("%1 / %2").arg(index + 1).arg(count));
            }

            QString m_documentId;
            QString m_sourceId;
            PreviewWidget* m_preview{nullptr};
            QSlider* m_slider{nullptr};
            QLabel* m_frameLabel{nullptr};
        };

        QString defaultTitle(const RecordingSessionData& session, const QString& cameraId)
        {
            const QString baseName = session.capturePlan().baseName.trimmed();
            return baseName.isEmpty() ? cameraId : baseName + QStringLiteral(" - ") + cameraId;
        }

        QString compactViewerTitle(const QString& title)
        {
            static const QRegularExpression timestampPattern(
                QStringLiteral("(\\d{8})_(\\d{6})"));
            const QRegularExpressionMatch firstMatch = timestampPattern.match(title);
            if (!firstMatch.hasMatch())
            {
                return title;
            }
            const QDateTime timestamp = QDateTime::fromString(
                firstMatch.captured(1) + QStringLiteral("_") + firstMatch.captured(2),
                QStringLiteral("yyyyMMdd_HHmmss"));
            if (!timestamp.isValid())
            {
                return title;
            }
            QString compact = title.left(firstMatch.capturedStart())
                + timestamp.toString(QStringLiteral("MM-dd HH:mm:ss"))
                + title.mid(firstMatch.capturedEnd());

            QRegularExpressionMatchIterator iterator = timestampPattern.globalMatch(compact);
            QList<QPair<int, int>> duplicateRanges;
            while (iterator.hasNext())
            {
                const QRegularExpressionMatch duplicate = iterator.next();
                duplicateRanges.append({duplicate.capturedStart(), duplicate.capturedLength()});
            }
            for (auto it = duplicateRanges.crbegin(); it != duplicateRanges.crend(); ++it)
            {
                compact.remove(it->first, it->second);
            }
            compact.remove(QRegularExpression(QStringLiteral("\\s*[-|_]\\s*$")));
            return compact.trimmed();
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
        QPointer<ImageDocumentPage> page;
    };

    ImageWorkspace::ImageWorkspace(scopeone::core::ScopeOneCore* core,
                                   QWidget* windowParent,
                                   QObject* parent)
        : QObject(parent), m_core(core)
    {
        m_viewerHost = new QWidget(windowParent);
        auto* hostLayout = new QVBoxLayout(m_viewerHost);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);

        m_viewerToolbar = new QToolBar(tr("Viewer"), m_viewerHost);
        m_viewerToolbar->setMovable(false);
        m_viewerToolbar->setFloatable(false);
        m_viewerToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
        hostLayout->addWidget(m_viewerToolbar);

        m_viewerStack = new QStackedWidget(m_viewerHost);
        m_viewerTabs = new QTabWidget(m_viewerStack);
        m_viewerTabs->setTabsClosable(true);
        m_viewerTabs->setDocumentMode(true);
        m_viewerStack->addWidget(m_viewerTabs);

        m_compareWidget = new QWidget(m_viewerStack);
        auto* compareLayout = new QHBoxLayout(m_compareWidget);
        compareLayout->setContentsMargins(0, 0, 0, 0);
        auto* compareSplitter = new QSplitter(Qt::Horizontal, m_compareWidget);
        m_compareLeftHost = new QGroupBox(m_compareWidget);
        m_compareLeftHost->setLayout(new QVBoxLayout);
        m_compareLeftHost->layout()->setContentsMargins(4, 4, 4, 4);
        m_compareRightHost = new QGroupBox(m_compareWidget);
        m_compareRightHost->setLayout(new QVBoxLayout);
        m_compareRightHost->layout()->setContentsMargins(4, 4, 4, 4);
        compareSplitter->addWidget(m_compareLeftHost);
        compareSplitter->addWidget(m_compareRightHost);
        compareSplitter->setSizes({1, 1});
        compareLayout->addWidget(compareSplitter);
        m_viewerStack->addWidget(m_compareWidget);
        hostLayout->addWidget(m_viewerStack, 1);

        setupViewerToolbar();
        const int viewerToolbarHeight = m_viewerToolbar->sizeHint().height();
        m_viewerToolbar->setMinimumHeight(viewerToolbarHeight);
        m_viewerToolbar->setMaximumHeight(viewerToolbarHeight);
        connect(m_viewerTabs, &QTabWidget::currentChanged,
                this, [this](int index)
                {
                    if (index == m_liveTabIndex)
                    {
                        activateLiveViewer();
                        return;
                    }
                    if (Document* document = findDocumentByPage(m_viewerTabs->widget(index)))
                    {
                        if (m_activeDocumentId != document->id)
                        {
                            setActiveDocument(document->id);
                        }
                        updateViewerToolbar();
                    }
                });
        connect(m_viewerTabs, &QTabWidget::tabCloseRequested,
                this, [this](int index)
                {
                    if (index == m_liveTabIndex)
                    {
                        return;
                    }
                    if (Document* document = findDocumentByPage(m_viewerTabs->widget(index)))
                    {
                        closeDocument(document->id);
                    }
                });

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
                    if (frame.isValid() && frameIndex == document->requestedFrameIndex)
                    {
                        document->frameIndex = frameIndex;
                        document->currentFrame = frame;
                        document->page->setFrameIndex(frameIndex);
                        document->page->showFrame(frame);
                        if (document->id == m_activeDocumentId)
                        {
                            emit activeFrameChanged();
                            updateLineProfile(*document);
                        }
                        emit documentsChanged();
                    }
                    if (document->frameIndex != document->requestedFrameIndex)
                    {
                        if (!requestFrame(*document, document->requestedFrameIndex))
                        {
                            document->page->setFrameIndex(document->frameIndex);
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
        m_viewerContextLabel = new QLabel(tr("Live Preview"), m_viewerToolbar);
        QFont contextFont = m_viewerContextLabel->font();
        contextFont.setBold(true);
        m_viewerContextLabel->setFont(contextFont);
        m_viewerContextLabel->setFixedWidth(180);
        m_viewerContextLabel->setToolTip(tr("Current image viewer"));
        m_viewerToolbar->addWidget(m_viewerContextLabel);
        m_viewerToolbar->addSeparator();

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

        m_viewerToolbar->addSeparator();
        m_viewerToolbar->addWidget(new QLabel(tr("Layer Layout"), m_viewerToolbar));
        m_layoutCombo = new QComboBox(m_viewerToolbar);
        m_layoutCombo->addItem(tr("Side by side"));
        m_layoutCombo->addItem(tr("Overlay"));
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
        m_compareAction = m_viewerToolbar->addAction(tr("Compare"));
        m_compareAction->setCheckable(true);
        m_compareAction->setToolTip(tr("Show two image documents side by side"));
        connect(m_compareAction, &QAction::toggled, this,
                [this](bool enabled)
                {
                    if (enabled)
                    {
                        if (!beginComparison())
                        {
                            const QSignalBlocker blocker(m_compareAction);
                            m_compareAction->setChecked(false);
                        }
                    }
                    else
                    {
                        endComparison();
                    }
                });

        m_compareDocumentCombo = new QComboBox(m_viewerToolbar);
        m_compareDocumentCombo->setMinimumWidth(180);
        m_compareDocumentCombo->setToolTip(tr("Choose the document shown on the right"));
        m_compareDocumentCombo->setVisible(false);
        m_viewerToolbar->addWidget(m_compareDocumentCombo);
        connect(m_compareDocumentCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index)
                {
                    if (!comparisonActive() || index < 0)
                    {
                        return;
                    }
                    QString rightId = m_compareDocumentCombo->itemData(index).toString();
                    const QString leftId = m_compareLeftDocumentId;
                    if (rightId == QStringLiteral("duplicate"))
                    {
                        rightId = duplicateDocument(leftId);
                    }
                    if (rightId.isEmpty() || rightId == m_compareRightDocumentId)
                    {
                        return;
                    }
                    endComparison();
                    setActiveDocument(leftId);
                    {
                        const QSignalBlocker blocker(m_compareAction);
                        m_compareAction->setChecked(true);
                    }
                    if (!beginComparison(rightId))
                    {
                        const QSignalBlocker blocker(m_compareAction);
                        m_compareAction->setChecked(false);
                    }
                });

        m_linkFramesAction = m_viewerToolbar->addAction(tr("Link Frames"));
        m_linkFramesAction->setCheckable(true);
        m_linkFramesAction->setToolTip(tr("Keep both compared sequences on the same frame number"));
        m_linkFramesAction->setVisible(false);
    }

    // Refreshes viewer controls when the active document or display state changes
    void ImageWorkspace::updateViewerToolbar()
    {
        PreviewWidget* preview = activePreviewWidget();
        const ImageDocumentInfo activeDocument = document();
        const QString contextTitle = activeDocument.isValid()
                                          ? activeDocument.title
                                          : tr("Live Preview");
        m_viewerContextLabel->setText(
            QFontMetrics(m_viewerContextLabel->font()).elidedText(
                contextTitle, Qt::ElideRight, m_viewerContextLabel->width()));
        m_viewerContextLabel->setToolTip(activeDocument.isValid()
                                             ? tr("Static image: %1").arg(contextTitle)
                                             : tr("Live camera viewer"));
        m_fitToWindowAction->setEnabled(preview != nullptr);
        m_oneToOneAction->setEnabled(preview != nullptr);
        m_compareAction->setEnabled(activeDocument.isValid());
        m_compareDocumentCombo->setVisible(comparisonActive());
        m_linkFramesAction->setVisible(comparisonActive());
        if (preview)
        {
            {
                const QSignalBlocker blocker(m_fitToWindowAction);
                m_fitToWindowAction->setChecked(preview->isFitToWindow());
            }
            {
                const QSignalBlocker blocker(m_layoutCombo);
                m_layoutCombo->setCurrentIndex(
                    preview->layerLayoutMode() == PreviewWidget::LayerLayoutMode::Overlay ? 1 : 0);
            }
        }

        {
            const QSignalBlocker blocker(m_compareDocumentCombo);
            m_compareDocumentCombo->clear();
            if (!m_compareLeftDocumentId.isEmpty())
            {
                const Document* left = findDocument(m_compareLeftDocumentId);
                m_compareDocumentCombo->addItem(
                    left ? tr("Same sequence, independent frame") : tr("Duplicate current document"),
                    QStringLiteral("duplicate"));
                for (const auto& candidate : m_documents)
                {
                    if (candidate->id != m_compareLeftDocumentId)
                    {
                        m_compareDocumentCombo->addItem(candidate->title, candidate->id);
                    }
                }
                const int rightIndex = m_compareDocumentCombo->findData(m_compareRightDocumentId);
                m_compareDocumentCombo->setCurrentIndex(rightIndex >= 0 ? rightIndex : 0);
            }
        }
    }

    void ImageWorkspace::setLiveViewer(PreviewWidget* previewWidget)
    {
        m_livePreviewWidget = previewWidget;
        connectViewer(previewWidget, {});
        m_liveLayerKey = previewWidget->visibleLayerKeys().value(0);
        if (m_liveTabIndex < 0)
        {
            m_liveTabIndex = m_viewerTabs->addTab(previewWidget, tr("Live Preview"));
            m_viewerTabs->tabBar()->setTabButton(
                m_liveTabIndex, QTabBar::RightSide, nullptr);
            m_viewerTabs->setCurrentIndex(m_liveTabIndex);
        }
        updateViewerToolbar();
    }

    void ImageWorkspace::activateLiveViewer()
    {
        if (!m_livePreviewWidget)
        {
            return;
        }
        if (comparisonActive())
        {
            endComparison();
        }
        m_viewerTabs->setCurrentIndex(m_liveTabIndex);
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
        scopeone::core::ImageSceneModel* scene = activeSceneModel();
        PreviewWidget* preview = activePreviewWidget();
        if (!scene || !preview)
        {
            return;
        }
        scene->setVisibleLayers(layerKeys);
        preview->setLayerLayoutMode(sideBySide
                                        ? PreviewWidget::LayerLayoutMode::SideBySide
                                        : PreviewWidget::LayerLayoutMode::Overlay);
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
            document->activeLayerKey = scopeone::core::ScopeOneCore::staticLayerKey(camera);
            document->page = new ImageDocumentPage(document->id,
                                                   document->title,
                                                   camera,
                                                   session,
                                                   session->experimentDocument(),
                                                   document->frameCount,
                                                   m_viewerTabs);
            connectViewer(document->page->previewWidget(), document->id);
            const QString id = document->id;
            connect(document->page, &ImageDocumentPage::frameIndexRequested,
                    this, &ImageWorkspace::requestDocumentFrame);
            m_documents.push_back(std::move(document));
            const int tabIndex = m_viewerTabs->addTab(
                m_documents.back()->page, compactViewerTitle(m_documents.back()->title));
            m_viewerTabs->setTabToolTip(tabIndex, m_documents.back()->title);
            if (!requestFrame(*m_documents.back(), 0))
            {
                closeDocument(id);
                continue;
            }
            openedIds.append(id);
        }
        if (!openedIds.isEmpty())
        {
            activateDocument(openedIds.constLast());
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
        if (documentId.trimmed().isEmpty())
        {
            return m_core->imageSceneModel();
        }
        const Document* document = findDocument(documentId);
        return document && document->page ? document->page->sceneModel() : nullptr;
    }

    PreviewWidget* ImageWorkspace::previewWidget(const QString& documentId) const
    {
        if (documentId.trimmed().isEmpty())
        {
            return m_livePreviewWidget;
        }
        const Document* document = findDocument(documentId);
        return document && document->page ? document->page->previewWidget() : nullptr;
    }

    scopeone::core::ImageFrame ImageWorkspace::frameForLayer(const QString& layerKey) const
    {
        if (isLiveViewerActive())
        {
            return m_core->graphFrame(layerKey);
        }
        const Document* document = findDocument(m_activeDocumentId);
        if (!document || !document->page
            || !document->page->sceneModel()->layerIds().contains(layerKey))
        {
            return {};
        }
        return document->currentFrame;
    }

    bool ImageWorkspace::histogram(
        const QString& layerKey,
        scopeone::core::ScopeOneCore::HistogramStats& stats) const
    {
        return isLiveViewerActive()
                   ? m_core->getLayerHistogram(layerKey, stats)
                   : scopeone::core::ScopeOneCore::computeHistogramStats(frameForLayer(layerKey), stats);
    }

    void ImageWorkspace::requestHistogram(const QString& layerKey)
    {
        queueHistogramRequest(layerKey, false);
    }

    // Queues a static frame histogram without blocking the viewer
    void ImageWorkspace::queueHistogramRequest(const QString& layerKey, bool applyAutoLevels)
    {
        if (isLiveViewerActive())
        {
            return;
        }
        const Document* document = findDocument(m_activeDocumentId);
        if (!document || !document->currentFrame.isValid())
        {
            return;
        }
        m_histogramDocumentId = document->id;
        m_histogramLayerKey = layerKey;
        m_histogramFrameIndex = document->frameIndex;
        m_histogramFrame = document->currentFrame;
        m_histogramApplyAutoLevels = applyAutoLevels;
        ++m_histogramGeneration;
        startHistogramRequest();
    }

    void ImageWorkspace::startHistogramRequest()
    {
        if (m_histogramRunning || !m_histogramFrame.isValid())
        {
            return;
        }
        m_histogramRunning = true;
        const QString documentId = m_histogramDocumentId;
        const QString layerKey = m_histogramLayerKey;
        const int frameIndex = m_histogramFrameIndex;
        const bool applyAutoLevels = m_histogramApplyAutoLevels;
        const quint64 generation = m_histogramGeneration;
        const scopeone::core::ImageFrame frame = std::exchange(
            m_histogramFrame, scopeone::core::ImageFrame{});
        m_histogramApplyAutoLevels = false;
        auto* watcher = new QFutureWatcher<scopeone::core::ScopeOneCore::HistogramStats>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, documentId, layerKey, frameIndex, applyAutoLevels, generation]()
                {
                    m_histogramRunning = false;
                    const Document* document = findDocument(documentId);
                    if (generation == m_histogramGeneration
                        && document && documentId == m_activeDocumentId
                        && document->frameIndex == frameIndex)
                    {
                        const auto stats = watcher->result();
                        if (applyAutoLevels)
                        {
                            activeSceneModel()->setLayerAutoStretchEnabled(layerKey, false);
                            activeSceneModel()->setLayerDisplayLevels(
                                layerKey,
                                stats.autoMinLevel,
                                stats.autoMaxLevel,
                                stats.maxValue);
                        }
                        emit histogramReady(layerKey, stats);
                    }
                    watcher->deleteLater();
                    startHistogramRequest();
                });
        watcher->setFuture(QtConcurrent::run([frame]()
        {
            scopeone::core::ScopeOneCore::HistogramStats stats;
            scopeone::core::ScopeOneCore::computeHistogramStats(frame, stats);
            return stats;
        }));
    }

    bool ImageWorkspace::autoLayerLevels(const QString& layerKey)
    {
        if (isLiveViewerActive())
        {
            return m_core->autoLayerLevels(layerKey);
        }
        if (!frameForLayer(layerKey).isValid())
        {
            return false;
        }
        queueHistogramRequest(layerKey, true);
        return true;
    }

    bool ImageWorkspace::fullLayerLevels(const QString& layerKey)
    {
        if (isLiveViewerActive())
        {
            return m_core->fullLayerLevels(layerKey);
        }
        const scopeone::core::ImageFrame frame = frameForLayer(layerKey);
        if (!frame.isValid())
        {
            return false;
        }
        activeSceneModel()->setLayerAutoStretchEnabled(layerKey, false);
        return activeSceneModel()->setLayerDisplayLevels(layerKey, 0, frame.maxValue(), frame.maxValue());
    }

    bool ImageWorkspace::setLayerAutoStretchEnabled(const QString& layerKey, bool enabled)
    {
        if (isLiveViewerActive())
        {
            return m_core->setLayerAutoStretchEnabled(layerKey, enabled);
        }
        if (!activeSceneModel()->setLayerAutoStretchEnabled(layerKey, enabled))
        {
            return false;
        }
        return !enabled || autoLayerLevels(layerKey);
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
        if (isLiveViewerActive())
        {
            return m_core->getLineProfile(layerKey, start, end, values);
        }
        const scopeone::core::ImageFrame frame = frameForLayer(layerKey);
        if (!frame.isValid())
        {
            return false;
        }
        const int count = qMax(qAbs(end.x() - start.x()), qAbs(end.y() - start.y())) + 1;
        values.clear();
        values.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            const double t = count == 1 ? 0.0 : static_cast<double>(i) / (count - 1);
            const int x = qRound(start.x() + t * (end.x() - start.x()));
            const int y = qRound(start.y() + t * (end.y() - start.y()));
            if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
            {
                return false;
            }
            const uchar* row = reinterpret_cast<const uchar*>(frame.bytes.constData())
                               + y * frame.stride;
            values.append(frame.bytesPerPixel() == 1
                              ? row[x]
                              : reinterpret_cast<const quint16*>(row)[x]);
        }
        return !values.isEmpty();
    }

    bool ImageWorkspace::pixelValue(const QString& layerKey, const QPoint& point, int& value) const
    {
        if (isLiveViewerActive())
        {
            return m_core->graphPixelValue(layerKey, point, value);
        }
        const scopeone::core::ImageFrame frame = frameForLayer(layerKey);
        if (!frame.isValid() || point.x() < 0 || point.y() < 0
            || point.x() >= frame.width || point.y() >= frame.height)
        {
            return false;
        }
        const uchar* row = reinterpret_cast<const uchar*>(frame.bytes.constData())
                           + point.y() * frame.stride;
        value = frame.bytesPerPixel() == 1
                    ? row[point.x()]
                    : reinterpret_cast<const quint16*>(row)[point.x()];
        return true;
    }

    double ImageWorkspace::pixelSizeUm(const QString& layerKey) const
    {
        if (isLiveViewerActive())
        {
            return m_core->cameraPixelSizeUm(
                scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey));
        }
        const Document* document = findDocument(m_activeDocumentId);
        return document ? document->session->cameraPixelSizeUm(document->cameraId) : 0.0;
    }

    QString ImageWorkspace::activeLayerKey() const
    {
        if (isLiveViewerActive())
        {
            const scopeone::core::ImageSceneModel* scene = activeSceneModel();
            return scene && scene->visibleLayerIds().contains(m_liveLayerKey)
                       ? m_liveLayerKey
                       : scene ? scene->visibleLayerIds().value(0) : QString{};
        }
        const Document* document = findDocument(m_activeDocumentId);
        scopeone::core::ImageSceneModel* scene = activeSceneModel();
        if (document && scene && scene->layerIds().contains(document->activeLayerKey))
        {
            return document->activeLayerKey;
        }
        return scene ? scene->visibleLayerIds().value(0) : QString{};
    }

    void ImageWorkspace::setActiveLayerKey(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        const QString previousLayerKey = activeLayerKey();
        if (isLiveViewerActive())
        {
            if (!activeSceneModel()->layerIds().contains(normalizedLayerKey))
            {
                return;
            }
            m_liveLayerKey = normalizedLayerKey;
        }
        else if (Document* document = findDocument(m_activeDocumentId))
        {
            if (!activeSceneModel()->layerIds().contains(normalizedLayerKey))
            {
                return;
            }
            document->activeLayerKey = normalizedLayerKey;
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
    void ImageWorkspace::syncActiveLayer(const QString& documentId)
    {
        scopeone::core::ImageSceneModel* scene = sceneModel(documentId);
        if (!scene)
        {
            return;
        }

        const QString previousLayerKey = documentId == m_activeDocumentId
                                             ? activeLayerKey()
                                             : QString{};
        const QStringList visibleLayerKeys = scene->visibleLayerIds();
        if (documentId.isEmpty())
        {
            if (!visibleLayerKeys.contains(m_liveLayerKey))
            {
                m_liveLayerKey = visibleLayerKeys.value(0);
            }
        }
        else if (Document* document = findDocument(documentId))
        {
            if (!visibleLayerKeys.contains(document->activeLayerKey))
            {
                document->activeLayerKey = visibleLayerKeys.value(0);
            }
        }

        updateViewerToolbar();
        if (documentId == m_activeDocumentId && previousLayerKey != activeLayerKey())
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
        if (!document || !document->page)
        {
            return false;
        }
        if (comparisonActive())
        {
            if (documentId == m_compareLeftDocumentId
                || documentId == m_compareRightDocumentId)
            {
                setActiveDocument(documentId);
                return true;
            }
            endComparison();
        }
        m_viewerTabs->setCurrentWidget(document->page);
        return true;
    }

    void ImageWorkspace::setActiveDocument(const QString& documentId)
    {
        if (m_activeDocumentId == documentId || !findDocument(documentId))
        {
            return;
        }
        m_activeDocumentId = documentId;
        if (comparisonActive())
        {
            const Document* left = findDocument(m_compareLeftDocumentId);
            const Document* right = findDocument(m_compareRightDocumentId);
            const QString leftTitle = left ? left->title : QString{};
            const QString rightTitle = right ? right->title : QString{};
            m_compareLeftHost->setTitle(documentId == m_compareLeftDocumentId
                                            ? tr("Active: %1").arg(leftTitle)
                                            : leftTitle);
            m_compareRightHost->setTitle(documentId == m_compareRightDocumentId
                                             ? tr("Active: %1").arg(rightTitle)
                                             : rightTitle);
        }
        emit activeDocumentChanged(documentId);
        emit activeViewerChanged();
        emit activeLayerChanged(activeLayerKey());
        emit documentsChanged();
        updateViewerToolbar();
    }

    bool ImageWorkspace::comparisonActive() const
    {
        return !m_compareLeftDocumentId.isEmpty()
               && !m_compareRightDocumentId.isEmpty();
    }

    bool ImageWorkspace::beginComparison(const QString& rightDocumentId)
    {
        Document* left = findDocument(m_activeDocumentId);
        if (!left || comparisonActive())
        {
            return false;
        }

        QString rightId = rightDocumentId;
        if (rightId.isEmpty())
        {
            for (const auto& candidate : m_documents)
            {
                if (candidate->id != left->id)
                {
                    rightId = candidate->id;
                    break;
                }
            }
        }
        if (rightId.isEmpty())
        {
            rightId = duplicateDocument(left->id);
        }
        Document* right = findDocument(rightId);
        if (!right || right == left)
        {
            return false;
        }

        m_compareLeftDocumentId = left->id;
        m_compareRightDocumentId = right->id;
        {
            const QSignalBlocker blocker(m_viewerTabs);
            const int leftIndex = m_viewerTabs->indexOf(left->page);
            if (leftIndex >= 0)
            {
                m_viewerTabs->removeTab(leftIndex);
            }
            const int rightIndex = m_viewerTabs->indexOf(right->page);
            if (rightIndex >= 0)
            {
                m_viewerTabs->removeTab(rightIndex);
            }
        }
        m_compareLeftHost->layout()->addWidget(left->page);
        m_compareRightHost->layout()->addWidget(right->page);
        m_viewerStack->setCurrentWidget(m_compareWidget);
        m_compareLeftHost->setTitle(tr("Active: %1").arg(left->title));
        m_compareRightHost->setTitle(right->title);
        updateViewerToolbar();
        return true;
    }

    void ImageWorkspace::endComparison()
    {
        if (!comparisonActive())
        {
            return;
        }
        Document* left = findDocument(m_compareLeftDocumentId);
        Document* right = findDocument(m_compareRightDocumentId);
        if (left && left->page)
        {
            m_compareLeftHost->layout()->removeWidget(left->page);
        }
        if (right && right->page)
        {
            m_compareRightHost->layout()->removeWidget(right->page);
        }
        m_compareLeftDocumentId.clear();
        m_compareRightDocumentId.clear();
        rebuildViewerTabs();
        m_viewerStack->setCurrentWidget(m_viewerTabs);
        {
            const QSignalBlocker blocker(m_compareAction);
            m_compareAction->setChecked(false);
        }
        updateViewerToolbar();
    }

    void ImageWorkspace::rebuildViewerTabs()
    {
        const QSignalBlocker blocker(m_viewerTabs);
        while (m_viewerTabs->count() > 0)
        {
            m_viewerTabs->removeTab(0);
        }
        m_liveTabIndex = m_livePreviewWidget
                             ? m_viewerTabs->addTab(m_livePreviewWidget, tr("Live Preview"))
                             : -1;
        if (m_liveTabIndex >= 0)
        {
            m_viewerTabs->tabBar()->setTabButton(m_liveTabIndex, QTabBar::RightSide, nullptr);
        }
        for (const auto& document : m_documents)
        {
            const int tabIndex = m_viewerTabs->addTab(
                document->page, compactViewerTitle(document->title));
            m_viewerTabs->setTabToolTip(tabIndex, document->title);
        }
        if (Document* active = findDocument(m_activeDocumentId))
        {
            m_viewerTabs->setCurrentWidget(active->page);
        }
        else if (m_liveTabIndex >= 0)
        {
            m_viewerTabs->setCurrentIndex(m_liveTabIndex);
        }
    }

    bool ImageWorkspace::closeDocument(const QString& documentId)
    {
        Document* document = findDocument(documentId);
        if (!document || !document->page)
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
                &document->page->sceneModel()->document()))
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
            document->page,
            tr("Select Dataset Folder"),
            QDir::homePath());
        if (saveDir.isEmpty())
        {
            return;
        }
        bool accepted = false;
        QString baseName = QInputDialog::getText(
                               document->page,
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

    ImageWorkspace::Document* ImageWorkspace::findDocumentByPage(QWidget* page) const
    {
        const auto it = std::find_if(m_documents.cbegin(), m_documents.cend(),
                                     [page](const auto& document)
                                     {
                                         return document->page == page;
                                     });
        return it == m_documents.cend() ? nullptr : it->get();
    }

    QString ImageWorkspace::duplicateDocument(const QString& documentId)
    {
        const Document* source = findDocument(documentId);
        if (!source || !source->session)
        {
            return {};
        }
        auto document = std::make_unique<Document>();
        document->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        document->title = tr("%1 Copy").arg(source->title);
        document->session = source->session;
        document->cameraId = source->cameraId;
        document->frameCount = source->frameCount;
        document->frameIndex = source->frameIndex;
        document->requestedFrameIndex = source->frameIndex;
        document->activeLayerKey = scopeone::core::ScopeOneCore::staticLayerKey(
            document->cameraId);
        document->page = new ImageDocumentPage(document->id,
                                               document->title,
                                               document->cameraId,
                                               document->session,
                                               document->session->experimentDocument(),
                                               document->frameCount,
                                               m_viewerTabs);
        connectViewer(document->page->previewWidget(), document->id);
        connect(document->page, &ImageDocumentPage::frameIndexRequested,
                this, &ImageWorkspace::requestDocumentFrame);
        const QString id = document->id;
        m_documents.push_back(std::move(document));
        const int tabIndex = m_viewerTabs->addTab(
            m_documents.back()->page, compactViewerTitle(m_documents.back()->title));
        m_viewerTabs->setTabToolTip(tabIndex, m_documents.back()->title);
        if (!requestFrame(*m_documents.back(), source->frameIndex))
        {
            removeDocument(id);
            return {};
        }
        emit documentsChanged();
        return id;
    }

    void ImageWorkspace::requestDocumentFrame(const QString& documentId, int frameIndex)
    {
        Document* document = findDocument(documentId);
        if (!document)
        {
            return;
        }
        requestFrame(*document, frameIndex);
        if (!comparisonActive() || !m_linkFramesAction->isChecked())
        {
            return;
        }
        const QString peerId = documentId == m_compareLeftDocumentId
                                   ? m_compareRightDocumentId
                                   : documentId == m_compareRightDocumentId
                                         ? m_compareLeftDocumentId
                                         : QString{};
        if (Document* peer = findDocument(peerId))
        {
            requestFrame(*peer, frameIndex);
        }
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
        if (comparisonActive()
            && (documentId == m_compareLeftDocumentId
                || documentId == m_compareRightDocumentId))
        {
            endComparison();
        }
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
        const int tabIndex = m_viewerTabs->indexOf((*it)->page);
        if (tabIndex >= 0)
        {
            m_viewerTabs->removeTab(tabIndex);
        }
        delete (*it)->page;
        m_documents.erase(it);
        if (m_activeDocumentId == documentId)
        {
            if (Document* active = findDocumentByPage(m_viewerTabs->currentWidget()))
            {
                m_activeDocumentId = active->id;
            }
            else
            {
                m_activeDocumentId.clear();
            }
            emit activeDocumentChanged(m_activeDocumentId);
            emit activeViewerChanged();
            emit activeLayerChanged(activeLayerKey());
            updateViewerToolbar();
        }
        emit documentsChanged();
    }

    void ImageWorkspace::connectViewer(PreviewWidget* preview, const QString& documentId)
    {
        connect(preview, &PreviewWidget::activated, this, [this, documentId]()
        {
            if (documentId.isEmpty())
            {
                activateLiveViewer();
            }
            else
            {
                setActiveDocument(documentId);
            }
        });
        connect(preview, &PreviewWidget::measurementLineDrawn,
                this, &ImageWorkspace::measurementLineDrawn);
        connect(preview, &PreviewWidget::measurementLineInspected,
                this, &ImageWorkspace::measurementLineInspected);
        connect(preview, &PreviewWidget::measurementLineCleared,
                this, &ImageWorkspace::measurementLineCleared);
        connect(preview, &PreviewWidget::layerClicked,
                this, [this](const QString& layerKey) { setActiveLayerKey(layerKey); });
        connect(preview, &PreviewWidget::mousePositionChanged,
                this, [this, documentId](const QPoint& position)
                {
                    if (documentId == m_activeDocumentId)
                    {
                        emit mousePositionChanged(position);
                    }
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
        connect(scene, &scopeone::core::ImageSceneModel::layersChanged,
                this, [this, documentId]() { syncActiveLayer(documentId); });
        if (documentId.isEmpty())
        {
            return;
        }
        connect(scene, &scopeone::core::ImageSceneModel::markupsChanged,
                this, [this, documentId]()
                {
                    Document* document = findDocument(documentId);
                    if (document && documentId == m_activeDocumentId)
                    {
                        updateLineProfile(*document);
                    }
                });
    }

    void ImageWorkspace::updateLineProfile(Document& document)
    {
        for (const auto& markup : document.page->sceneModel()->markups())
        {
            if (markup.role == scopeone::core::ImageSceneModel::MarkupRole::CrossSection)
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

#include "ImageWorkspace.moc"
