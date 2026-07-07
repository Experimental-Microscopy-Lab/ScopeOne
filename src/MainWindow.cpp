#include "MainWindow.h"

#include "scopeone/ScopeOneCore.h"
#include "AboutDialog.h"
#include "InspectWidget.h"
#include "ConsoleWidget.h"
#include "DeviceControlWidget.h"
#include "DevicePropertyWidget.h"
#include "ConfigPresetWidget.h"
#include "PreviewWidget.h"
#include "ImageGalleryWidget.h"
#include "ImageToolsDialog.h"
#include "ImageProcessingWidget.h"
#include "RecordingWidget.h"
#include "SettingsDialog.h"
#include "ScopeOneLocalApiServer.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVector>
#include <memory>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        // Build raw layer keys for all camera ids
        QStringList rawLayerKeys(const QStringList& cameraIds)
        {
            QStringList layerKeys;
            layerKeys.reserve(cameraIds.size());
            for (const QString& cameraId : cameraIds)
            {
                layerKeys.append(scopeone::core::ScopeOneCore::rawLayerKey(cameraId));
            }
            return layerKeys;
        }

        // Keep only raw layers from the current preview selection
        QStringList rawOnlyLayerKeys(const QStringList& layerKeys)
        {
            QStringList rawKeys;
            rawKeys.reserve(layerKeys.size());
            for (const QString& layerKey : layerKeys)
            {
                if (scopeone::core::ScopeOneCore::isRawLayerKey(layerKey))
                {
                    rawKeys.append(layerKey);
                }
            }
            return rawKeys;
        }

        // Apply common status label presentation
        void configureStatusLabel(QLabel* label, int minWidth, int maxWidth, const QString& tooltip)
        {
            label->setTextFormat(Qt::PlainText);
            label->setMinimumWidth(minWidth);
            if (maxWidth > 0)
            {
                label->setMaximumWidth(maxWidth);
            }
            label->setToolTip(tooltip);
            label->setStyleSheet(QStringLiteral("padding: 0 8px;"));
        }

        // Update a status label without redundant repaints
        void setStatusLabelText(QLabel* label, const QString& text, const QString& tooltip)
        {
            if (label->text() != text)
            {
                label->setText(text);
            }
            if (label->toolTip() != tooltip)
            {
                label->setToolTip(tooltip);
            }
        }

        // Build a stable in process key for one gallery session
        QString gallerySessionKey(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return QString::number(
                static_cast<qulonglong>(reinterpret_cast<quintptr>(session.get())), 16);
        }

        // Build the static layer id for one recorded camera in a gallery session
        QString gallerySessionLayerId(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& cameraId)
        {
            return QStringLiteral("%1_%2").arg(gallerySessionKey(session), cameraId);
        }

        // Publish the first frame of each recorded camera as graph layers
        QStringList previewGallerySession(
            scopeone::core::ScopeOneCore& core,
            PreviewWidget& previewWidget,
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            QStringList selectedLayerKeys = previewWidget.selectedLayerKeys();
            QStringList openedLayerKeys;

            for (const scopeone::core::ImageFrame& frame : core.firstSessionFrames(session))
            {
                const QString cameraId = frame.cameraId;

                const QString displayName = session->recordedFrameCount(cameraId) > 1
                                                ? QStringLiteral("Gallery %1 Frame 1").arg(cameraId)
                                                : QStringLiteral("Gallery %1").arg(cameraId);
                const QString layerId = gallerySessionLayerId(session, cameraId);
                const scopeone::core::ImageFrame graphFrame = core.publishStaticFrame(layerId, frame, displayName);
                if (!graphFrame.isValid())
                {
                    continue;
                }
                const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(layerId);
                if (!layerKey.isEmpty())
                {
                    openedLayerKeys.append(layerKey);
                    if (!selectedLayerKeys.contains(layerKey))
                    {
                        selectedLayerKeys.append(layerKey);
                    }
                }
            }

            if (!openedLayerKeys.isEmpty())
            {
                previewWidget.setSelectedLayerKeys(selectedLayerKeys);
                previewWidget.setLayerLayoutMode(selectedLayerKeys.size() > 1
                                                     ? PreviewWidget::LayerLayoutMode::SideBySide
                                                     : PreviewWidget::LayerLayoutMode::Overlay);
            }
            return openedLayerKeys;
        }

        // Remove graph layers that belong to one gallery session
        void removeGallerySessionPreview(
            scopeone::core::ScopeOneCore& core,
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            for (const QString& cameraId : session->recordedCameraIds())
            {
                core.removeStaticFrame(gallerySessionLayerId(session, cameraId));
            }
        }

        // Return the current recording save result if one exists
        QString recordingSessionMessage(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return session ? session->saveMessage() : QStringLiteral("Error: no session data");
        }
    }

    // Build the main shell around the shared core facade
    MainWindow::MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QMainWindow(parent)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("MainWindow requires ScopeOneCore");
        }

        setupUI();
        new ScopeOneLocalApiServer(m_scopeonecore, this);
        setupSignalWiring();
        applyStoredApplicationSettings();
        logStartupSummary();

        setWindowTitle("ScopeOne");
        setMinimumSize(1366, 768);
        resize(1600, 900);
    }

    // Confirm what to do with unsaved gallery sessions
    void MainWindow::closeEvent(QCloseEvent* event)
    {
        const auto unsavedSessions = m_imageGalleryWidget->unsavedSessions();
        if (unsavedSessions.isEmpty())
        {
            QMainWindow::closeEvent(event);
            return;
        }

        const QMessageBox::StandardButton reply = QMessageBox::warning(
            this,
            tr("Unsaved Gallery Images"),
            tr("There are %1 unsaved gallery item(s). Save them before closing?")
            .arg(unsavedSessions.size()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (reply == QMessageBox::Cancel)
        {
            event->ignore();
            return;
        }

        if (reply == QMessageBox::Save)
        {
            for (const auto& session : unsavedSessions)
            {
                const QString result = m_scopeonecore->saveRecordingSession(session);
                if (!session->isSaved())
                {
                    QMessageBox::critical(
                        this,
                        tr("Save Failed"),
                        result.isEmpty() ? tr("Failed to save gallery images") : result);
                    event->ignore();
                    return;
                }
                m_imageGalleryWidget->markSessionSaved(session);
            }
        }

        QMainWindow::closeEvent(event);
    }

    // Connect core events and panel actions into one UI flow
    void MainWindow::setupSignalWiring()
    {
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewStateChanged,
                this, [this](bool running)
                {
                    m_deviceControlWidget->setPreviewRunning(running);
                    m_deviceControlWidget->setControlTargetEnabled(!running);
                    setStatusLabelText(m_statusPreviewLabel,
                                       running ? tr("Preview: Live") : tr("Preview: Idle"),
                                       running ? tr("Preview is running") : tr("Preview is idle"));
                    showStatusMessage(running ? tr("Live preview started") : tr("Live preview stopped"), 3000);
                });

        connect(m_previewWidget, &PreviewWidget::mousePositionChanged,
                this, &MainWindow::handlePreviewMousePosition);
        connect(m_previewWidget, &PreviewWidget::roiDrawn,
                this, &MainWindow::handleRoiDrawn);
        connect(m_previewWidget, &PreviewWidget::lineDrawn,
                this, [this](const QString& layerKey,
                             const QString& sourceId,
                             int startX,
                             int startY,
                             int endX,
                             int endY,
                             bool processed)
                {
                    const QPoint start(startX, startY);
                    const QPoint end(endX, endY);
                    if (scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey))
                    {
                        m_scopeonecore->setStaticLineProfile(sourceId, start, end);
                        return;
                    }

                    m_scopeonecore->setLineProfile(sourceId,
                                                   start,
                                                   end,
                                                   processed);
                });

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewRawFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    if (!frame.isValid())
                    {
                        return;
                    }
                    m_previewWidget->setGraphRawFrame(frame);
                    refreshPreviewCursorStatus();
                });

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewProcessedFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setGraphProcessedFrame(frame);
                    refreshPreviewCursorStatus();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFramePublished,
                this, [this](const QString& sourceId,
                             const QString& displayName,
                             const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setGraphStaticLayerFrame(sourceId, displayName, frame);
                    refreshPreviewCursorStatus();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFrameRemoved,
                this, [this](const QString& sourceId)
                {
                    m_previewWidget->removeStaticLayer(
                        scopeone::core::ScopeOneCore::staticLayerKey(sourceId));
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFramesCleared,
                m_previewWidget, &PreviewWidget::clearStaticLayers);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::liveFramesCleared,
                m_previewWidget, &PreviewWidget::clearSourceFrames);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processedFramesCleared,
                m_previewWidget, &PreviewWidget::clearProcessedFrames);

        connect(m_deviceControlWidget, &DeviceControlWidget::startPreviewRequested,
                this, [this]()
                {
                    m_scopeonecore->startPreview(m_currentControlTarget);
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::stopPreviewRequested,
                this, [this]()
                {
                    m_scopeonecore->stopPreview(m_currentControlTarget);
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::requestDrawROI,
                this, [this](const QString& cameraId)
                {
                    m_previewWidget->startROIDrawing(cameraId);
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::requestHalfROI,
                this, [this](const QString& cameraId)
                {
                    int originX = 0;
                    int originY = 0;
                    int sourceWidth = 0;
                    int sourceHeight = 0;
                    if (!m_scopeonecore->getROI(cameraId, originX, originY, sourceWidth, sourceHeight)
                        || sourceWidth <= 0
                        || sourceHeight <= 0)
                    {
                        scopeone::core::ImageFrame latestFrame;
                        if (!m_scopeonecore->getLatestRawFrame(cameraId, latestFrame) || !latestFrame.isValid())
                        {
                            qWarning().noquote() << QString("Failed to set half ROI for %1: no raw frame").arg(cameraId);
                            showStatusMessage(tr("No raw frame available for Half ROI"), 5000);
                            return;
                        }
                        originX = 0;
                        originY = 0;
                        sourceWidth = latestFrame.width;
                        sourceHeight = latestFrame.height;
                    }

                    const int roiWidth = qMax(1, sourceWidth / 2);
                    const int roiHeight = qMax(1, sourceHeight / 2);
                    const int x = originX + (sourceWidth - roiWidth) / 2;
                    const int y = originY + (sourceHeight - roiHeight) / 2;
                    if (m_scopeonecore->setROI(cameraId, x, y, roiWidth, roiHeight))
                    {
                        m_scopeonecore->clearLiveFrames(cameraId);
                        qInfo().noquote() << QString("Half ROI set for %1: %2x%3 at (%4,%5)")
                                             .arg(cameraId)
                                             .arg(roiWidth)
                                             .arg(roiHeight)
                                             .arg(x)
                                             .arg(y);
                    }
                    else
                    {
                        showStatusMessage(tr("Failed to set Half ROI"), 5000);
                        qWarning().noquote() << QString("Failed to set half ROI for %1").arg(cameraId);
                    }
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::requestClearROI,
                this, [this](const QString& cameraId)
                {
                    const QString target = cameraId.trimmed();
                    if (target.isEmpty())
                    {
                        return;
                    }
                    QStringList cameraIds;
                    if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
                    {
                        cameraIds = m_scopeonecore->cameraIds();
                    }
                    else
                    {
                        cameraIds << target;
                    }

                    for (const QString& id : cameraIds)
                    {
                        if (m_scopeonecore->clearROI(id))
                        {
                            m_scopeonecore->clearLiveFrames(id);
                            qInfo().noquote() << QString("ROI restored for %1").arg(id);
                        }
                        else
                        {
                            showStatusMessage(tr("Failed to restore ROI"), 5000);
                            qWarning().noquote() << QString("Failed to restore ROI for %1").arg(id);
                        }
                    }
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::controlTargetChanged,
                this, &MainWindow::updateControlTarget);
        connect(m_deviceControlWidget, &DeviceControlWidget::currentLayerChanged,
                m_inspectWidget, &InspectWidget::setCurrentLayer);
        connect(m_previewWidget, &PreviewWidget::availableLayerKeysChanged,
                m_inspectWidget, &InspectWidget::setAvailableLayers);
        connect(m_deviceControlWidget, &DeviceControlWidget::exposureValueChanged,
                this, [this](double ms)
                {
                    if (!m_scopeonecore->setExposure(m_currentControlTarget, ms))
                    {
                        showStatusMessage(tr("Failed to set exposure"), 5000);
                        qWarning().noquote() << QString("Failed to set exposure: %1 ms").arg(ms);
                    }
                    m_deviceControlWidget->refreshCameraParameters();
                    m_propertyBrowser->refresh(false);
                });

        connect(m_inspectWidget, &InspectWidget::requestDrawCrossSectionLayer,
                this, [this](const QString& layerKey)
                {
                    m_previewWidget->startLineDrawingForLayer(layerKey);
                    showStatusMessage(tr("Drag a line on the preview"), 5000);
                });

        connect(m_inspectWidget, &InspectWidget::requestClearCrossSection,
                this, [this]()
                {
                    m_previewWidget->clearLine();
                    m_scopeonecore->clearLineProfile();
                });

        connect(m_inspectWidget, &InspectWidget::displayRangeChanged,
                m_previewWidget, &PreviewWidget::setLayerDisplayLevels);

        m_inspectWidget->setAvailableLayers(m_previewWidget->availableLayerKeys());
        m_inspectWidget->setCurrentLayer(m_deviceControlWidget->currentLayerKey());

        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStarted,
                this, [this]()
                {
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Live"),
                                       tr("Processing is running"));
                    showStatusMessage(tr("Image processing started"), 3000);
                    QStringList selectedLayerKeys = m_previewWidget->selectedLayerKeys();
                    const QStringList availableCameraIds = m_previewWidget->availableCameraIds();

                    for (const QString& layerKey : std::as_const(selectedLayerKeys))
                    {
                        if (!scopeone::core::ScopeOneCore::isRawLayerKey(layerKey))
                        {
                            continue;
                        }
                        const QString cameraId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
                        if (cameraId.isEmpty() || !availableCameraIds.contains(cameraId))
                        {
                            continue;
                        }
                        const QString processedLayerKey = scopeone::core::ScopeOneCore::processedLayerKey(cameraId);
                        if (!selectedLayerKeys.contains(processedLayerKey))
                        {
                            selectedLayerKeys.append(processedLayerKey);
                        }
                    }

                    if (selectedLayerKeys.isEmpty())
                    {
                        for (const QString& cameraId : availableCameraIds)
                        {
                            selectedLayerKeys.append(scopeone::core::ScopeOneCore::processedLayerKey(cameraId));
                        }
                    }

                    m_previewWidget->setSelectedLayerKeys(selectedLayerKeys);
                    m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::SideBySide);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStopped,
                this, [this]()
                {
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Off"),
                                       tr("Processing is off"));
                    showStatusMessage(tr("Image processing stopped"), 3000);
                    QStringList selectedLayerKeys = rawOnlyLayerKeys(m_previewWidget->selectedLayerKeys());
                    if (selectedLayerKeys.isEmpty())
                    {
                        selectedLayerKeys = rawLayerKeys(m_previewWidget->availableCameraIds());
                    }
                    m_previewWidget->setSelectedLayerKeys(selectedLayerKeys);
                    m_scopeonecore->clearProcessedFrames();
                });

        connect(m_exitAction, &QAction::triggered, this, &QWidget::close);
        connect(m_fullScreenAction, &QAction::toggled,
                this, &MainWindow::setFullScreenEnabled);
        connect(m_aboutAction, &QAction::triggered,
                this, [this]() { AboutDialog::showAbout(this); });
        connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
        connect(m_loadConfigurationAction, &QAction::triggered,
                this, &MainWindow::loadConfigurationFromDialog);
        connect(m_unloadConfigurationAction, &QAction::triggered,
                this, &MainWindow::unloadConfigurationWithConfirmation);
        connect(m_stageMosaicAction, &QAction::triggered,
                this, &MainWindow::openStageMosaicTool);
        connect(m_particleDetectionAction, &QAction::triggered,
                this, &MainWindow::openParticleDetectionTool);
        connect(m_settingsAction, &QAction::triggered,
                this, &MainWindow::openSettingsDialog);

        connect(m_recordingWidget, &RecordingWidget::gallerySessionCaptured,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_imageGalleryWidget->addSession(session);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_imageGalleryWidget->addSession(session);
                    const QString result = recordingSessionMessage(session);
                    if (!result.isEmpty())
                    {
                        showStatusMessage(result, session && session->isSaved() ? 5000 : 8000);
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStateChanged,
                this, [this](bool recording)
                {
                    setStatusLabelText(m_statusRecordingLabel,
                                       recording ? tr("Recording: Active") : tr("Recording: Idle"),
                                       recording ? tr("Recording is active") : tr("Recording is idle"));
                    showStatusMessage(recording ? tr("Recording started") : tr("Recording stopped"), 3000);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionSaveFinished,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    if (session && session->isSaved())
                    {
                        m_imageGalleryWidget->markSessionSaved(session);
                    }
                    const QString result = recordingSessionMessage(session);
                    if (!result.isEmpty())
                    {
                        showStatusMessage(result, session && session->isSaved() ? 5000 : 8000);
                    }
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionOpenRequested,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    const QStringList selectedLayerKeys = previewGallerySession(
                        *m_scopeonecore,
                        *m_previewWidget,
                        session);
                    if (selectedLayerKeys.isEmpty())
                    {
                        showStatusMessage(tr("No gallery image available for preview"), 5000);
                        return;
                    }
                    showStatusMessage(
                        tr("Gallery preview opened with %1 layer(s)").arg(selectedLayerKeys.size()),
                        5000);
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::livePreviewRequested,
                this, &MainWindow::showLivePreview);
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionRemoved,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    removeGallerySessionPreview(*m_scopeonecore, session);
                    m_scopeonecore->removeSessionFrameSource(session);
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::saveSessionsRequested,
                this,
                [this](const QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>>& sessions)
                {
                    for (const auto& session : sessions)
                    {
                        if (session)
                        {
                            m_scopeonecore->saveRecordingSessionAsync(session);
                        }
                    }
                });

        connectPropertyPanels();
    }

    // Connect property and preset panels to logging and refresh actions
    void MainWindow::connectPropertyPanels()
    {
        connect(m_propertyBrowser, &DevicePropertyWidget::propertyChanged,
                this, [this](const QString& device, const QString& property, const QString& value)
                {
                    const QString message = QStringLiteral("Property changed: %1.%2 = %3")
                                            .arg(device, property, value);
                    showStatusMessage(message, 3000);
                    qInfo().noquote() << message;
                    m_deviceControlWidget->refreshCameraParameters();
                });
        connect(m_propertyBrowser, &DevicePropertyWidget::errorOccurred,
                this, [this](const QString& message)
                {
                    showStatusMessage(message, 5000);
                    qCritical().noquote() << message;
                });
        connect(m_configPresetWidget, &ConfigPresetWidget::configChanged,
                this, [](const QString& group, const QString& preset)
                {
                    qInfo().noquote() << QString("Config changed: %1 = %2").arg(group, preset);
                });
        connect(m_configPresetWidget, &ConfigPresetWidget::errorOccurred,
                this, [this](const QString& message)
                {
                    showStatusMessage(message, 5000);
                    qCritical().noquote() << message;
                });
    }

    // Create the preview area and all docked tool panels
    void MainWindow::setupUI()
    {
        m_previewWidget = new PreviewWidget(this);
        setCentralWidget(m_previewWidget);

        setupStatusBar();
        setupDeviceControl();
        setupInspect();
        setupImageProcessing();
        setupConsole();
        setupMenuBar();
        setupPropertyBrowser();
        setupRecording();
        setupImageGallery();
        updateDockWidgetMenu();
    }

    // Create the shared status strip for transient and persistent state
    void MainWindow::setupStatusBar()
    {
        auto* bar = statusBar();
        bar->setSizeGripEnabled(false);

        m_statusMessageLabel = new QLabel(tr("Ready"), this);
        configureStatusLabel(m_statusMessageLabel, 260, 0, tr("Latest operation message"));

        m_statusCursorLabel = new QLabel(tr("Cursor: -"), this);
        configureStatusLabel(m_statusCursorLabel, 300, 360, tr("Pixel readout"));

        m_statusTargetLabel = new QLabel(tr("Target: All"), this);
        configureStatusLabel(m_statusTargetLabel, 110, 160, tr("Current control target"));

        m_statusPreviewLabel = new QLabel(tr("Preview: Idle"), this);
        configureStatusLabel(m_statusPreviewLabel, 100, 130, tr("Preview state"));

        m_statusProcessingLabel = new QLabel(tr("Processing: Off"), this);
        configureStatusLabel(m_statusProcessingLabel, 120, 150, tr("Processing state"));

        m_statusRecordingLabel = new QLabel(tr("Recording: Idle"), this);
        configureStatusLabel(m_statusRecordingLabel, 120, 150, tr("Recording state"));

        bar->addWidget(m_statusMessageLabel, 1);
        bar->addPermanentWidget(m_statusCursorLabel);
        bar->addPermanentWidget(m_statusTargetLabel);
        bar->addPermanentWidget(m_statusPreviewLabel);
        bar->addPermanentWidget(m_statusProcessingLabel);
        bar->addPermanentWidget(m_statusRecordingLabel);

        m_statusMessageTimer = new QTimer(this);
        m_statusMessageTimer->setSingleShot(true);
        connect(m_statusMessageTimer, &QTimer::timeout, this, [this]()
        {
            setStatusLabelText(m_statusMessageLabel, tr("Ready"), tr("Latest operation message"));
        });
    }

    // Create application menus and persistent actions
    void MainWindow::setupMenuBar()
    {
        m_fileMenu = menuBar()->addMenu(tr("&File"));
        m_loadConfigurationAction = m_fileMenu->addAction(tr("&Load Configuration..."));
        m_unloadConfigurationAction = m_fileMenu->addAction(tr("&Unload Configuration"));
        m_fileMenu->addSeparator();
        m_exitAction = m_fileMenu->addAction(tr("E&xit"));

        m_viewMenu = menuBar()->addMenu(tr("&View"));
        m_fullScreenAction = m_viewMenu->addAction(tr("&Full Screen"));
        m_fullScreenAction->setCheckable(true);
        m_viewMenu->addSeparator();
        m_dockWidgetsMenu = m_viewMenu->addMenu(tr("&Dock Widgets"));

        m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
        m_stageMosaicAction = m_toolsMenu->addAction(tr("Stage &Mosaic..."));
        m_particleDetectionAction = m_toolsMenu->addAction(tr("&Particle Detection..."));
        m_toolsMenu->addSeparator();
        m_settingsAction = m_toolsMenu->addAction(tr("&Settings..."));

        m_helpMenu = menuBar()->addMenu(tr("&Help"));
        m_aboutQtAction = m_helpMenu->addAction(tr("About &Qt"));
        m_aboutAction = m_helpMenu->addAction(tr("&About ScopeOne"));
    }

    // Create the camera control dock
    void MainWindow::setupDeviceControl()
    {
        m_deviceControlDockWidget = new QDockWidget(tr("Control"), this);
        m_deviceControlWidget = new DeviceControlWidget(m_scopeonecore, this);
        m_deviceControlWidget->setPreviewWidget(m_previewWidget);
        m_deviceControlDockWidget->setWidget(m_deviceControlWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_deviceControlDockWidget);
    }

    // Create the image inspection dock
    void MainWindow::setupInspect()
    {
        m_inspectDockWidget = new QDockWidget(tr("Inspect"), this);
        m_inspectDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_inspectWidget = new InspectWidget(m_scopeonecore, this);
        m_inspectDockWidget->setWidget(m_inspectWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_inspectDockWidget);
        tabifyDockWidget(m_deviceControlDockWidget, m_inspectDockWidget);
    }

    // Create the processing module dock
    void MainWindow::setupImageProcessing()
    {
        m_imageProcessingDockWidget = new QDockWidget(tr("Image Processing"), this);
        m_imageProcessingDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_imageProcessingWidget = new ImageProcessingWidget(m_scopeonecore, this);
        m_imageProcessingDockWidget->setWidget(m_imageProcessingWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_imageProcessingDockWidget);
        tabifyDockWidget(m_inspectDockWidget, m_imageProcessingDockWidget);
        m_deviceControlDockWidget->raise();
    }

    // Create the log console dock and install the Qt message sink
    void MainWindow::setupConsole()
    {
        m_consoleDockWidget = new QDockWidget(tr("Console"), this);
        m_consoleWidget = new ConsoleWidget(this);
        m_consoleDockWidget->setWidget(m_consoleWidget);

        ConsoleWidget::installAsQtMessageSink(m_consoleWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_consoleDockWidget);
        splitDockWidget(m_deviceControlDockWidget, m_consoleDockWidget, Qt::Vertical);
    }

    // Create the device property and config preset dock
    void MainWindow::setupPropertyBrowser()
    {
        m_propertyDockWidget = new QDockWidget(tr("Device Properties"), this);
        m_propertyDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

        m_propertyBrowser = new DevicePropertyWidget(m_scopeonecore, this);
        m_configPresetWidget = new ConfigPresetWidget(m_scopeonecore, this);

        auto* tabWidget = new QTabWidget(m_propertyDockWidget);
        tabWidget->addTab(m_propertyBrowser, tr("Properties"));
        tabWidget->addTab(m_configPresetWidget, tr("Configs"));
        m_propertyDockWidget->setWidget(tabWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_propertyDockWidget);
    }

    // Create the recording control dock
    void MainWindow::setupRecording()
    {
        m_recordingDockWidget = new QDockWidget(tr("Recording"), this);
        m_recordingDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

        m_recordingWidget = new RecordingWidget(m_scopeonecore, this);
        m_recordingDockWidget->setWidget(m_recordingWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_recordingDockWidget);
        splitDockWidget(m_propertyDockWidget, m_recordingDockWidget, Qt::Vertical);
    }

    // Create the image gallery dock
    void MainWindow::setupImageGallery()
    {
        m_imageGalleryDockWidget = new QDockWidget(tr("Image Gallery"), this);
        m_imageGalleryDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        m_imageGalleryDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_imageGalleryWidget = new ImageGalleryWidget(this);
        m_imageGalleryDockWidget->setWidget(m_imageGalleryWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_imageGalleryDockWidget);
        tabifyDockWidget(m_recordingDockWidget, m_imageGalleryDockWidget);
        m_recordingDockWidget->raise();
    }

    // Close the modal configuration progress dialog if present
    void MainWindow::closeLoadConfigProgress()
    {
        if (!m_loadConfigProgress)
        {
            return;
        }

        m_loadConfigProgress->close();
        m_loadConfigProgress->deleteLater();
        m_loadConfigProgress = nullptr;
    }

    // Shows live raw camera layers in the shared preview
    void MainWindow::showLivePreview()
    {
        const QStringList cameraIds = m_scopeonecore->cameraIds();
        if (cameraIds.isEmpty())
        {
            showStatusMessage(tr("No live camera available"), 5000);
            return;
        }

        m_scopeonecore->clearStaticFrames();

        if (m_currentControlTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_previewWidget->setSelectedLayerKeys(rawLayerKeys(cameraIds));
            m_previewWidget->setLayerLayoutMode(cameraIds.size() > 1
                                                    ? PreviewWidget::LayerLayoutMode::SideBySide
                                                    : PreviewWidget::LayerLayoutMode::Overlay);
            return;
        }

        if (cameraIds.contains(m_currentControlTarget))
        {
            m_previewWidget->setSelectedLayerKeys({scopeone::core::ScopeOneCore::rawLayerKey(m_currentControlTarget)});
            m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
            return;
        }

        m_previewWidget->setSelectedLayerKeys(rawLayerKeys(cameraIds));
        m_previewWidget->setLayerLayoutMode(cameraIds.size() > 1
                                                ? PreviewWidget::LayerLayoutMode::SideBySide
                                                : PreviewWidget::LayerLayoutMode::Overlay);
    }

    // Switch the active camera target and preview selection
    void MainWindow::updateControlTarget(const QString& target)
    {
        // Keep preview on the chosen target
        const QString normalizedTarget = target.trimmed();
        if (normalizedTarget.isEmpty())
        {
            return;
        }
        m_currentControlTarget = normalizedTarget;
        setStatusLabelText(m_statusTargetLabel,
                           tr("Target: %1").arg(normalizedTarget),
                           tr("Current control target: %1").arg(normalizedTarget));

        showLivePreview();

        const QStringList cameraIds = m_scopeonecore->cameraIds();
        if (normalizedTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            return;
        }

        // Keep only one live preview
        for (const QString& id : cameraIds)
        {
            if (id == normalizedTarget)
            {
                continue;
            }
            m_scopeonecore->stopPreview(id);
            m_scopeonecore->clearLiveFrames(id);
        }

    }

    // Rebuild the view menu from current dock widgets
    void MainWindow::updateDockWidgetMenu()
    {
        m_dockWidgetsMenu->clear();
        const auto addDock = [this](QDockWidget* dockWidget, const QString& label)
        {
            QAction* action = dockWidget->toggleViewAction();
            action->setText(label);
            m_dockWidgetsMenu->addAction(action);
        };

        addDock(m_propertyDockWidget, QStringLiteral("Device Properties"));
        addDock(m_recordingDockWidget, QStringLiteral("Recording"));
        addDock(m_imageGalleryDockWidget, QStringLiteral("Image Gallery"));
        addDock(m_consoleDockWidget, QStringLiteral("Console"));
        addDock(m_deviceControlDockWidget, QStringLiteral("Control"));
        addDock(m_inspectDockWidget, QStringLiteral("Inspect"));
        addDock(m_imageProcessingDockWidget, QStringLiteral("Image Processing"));
    }

    // Push loaded camera ids into every dependent panel
    void MainWindow::applyLoadedCameraState(const QStringList& cameraIds)
    {
        m_deviceControlWidget->onCameraInitialized(true);
        m_deviceControlWidget->setControlTargets(cameraIds);
        m_inspectWidget->onCameraInitialized(true);
        m_inspectWidget->setAvailableCameras(cameraIds);

        m_previewWidget->setAvailableCameraIds(cameraIds);
        setStatusLabelText(m_statusTargetLabel,
                           tr("Target: All"),
                           tr("Current control target: All"));

        if (cameraIds.size() > 1)
        {
            m_previewWidget->setSelectedLayerKeys(rawLayerKeys(cameraIds));
        }
        else if (!cameraIds.isEmpty())
        {
            m_previewWidget->setSelectedLayerKeys({scopeone::core::ScopeOneCore::rawLayerKey(cameraIds.first())});
        }

        m_recordingWidget->setAvailableCameras(cameraIds);
    }

    // Clear preview and panel state after devices unload
    void MainWindow::applyUnloadedCameraState(const QStringList& cameraIds)
    {
        (void)cameraIds;
        m_previewWidget->setAvailableCameraIds({});
        setStatusLabelText(m_statusTargetLabel,
                           tr("Target: All"),
                           tr("Current control target: All"));
        setStatusLabelText(m_statusPreviewLabel,
                           tr("Preview: Idle"),
                           tr("Preview is idle"));
        clearCursorStatus();
        m_previewWidget->clearLine();
        m_deviceControlWidget->setControlTargets({});
        m_deviceControlWidget->onCameraInitialized(false);
        m_inspectWidget->setAvailableCameras({});
        m_inspectWidget->onCameraInitialized(false);
        m_inspectWidget->clearCrossSectionProfile();

        m_recordingWidget->setAvailableCameras({});
        showStatusMessage(tr("Configuration unloaded"), 3000);
    }

    // Refresh panels that mirror device state
    void MainWindow::refreshDevicePanels(bool fromCache)
    {
        m_propertyBrowser->refresh(fromCache);
        m_configPresetWidget->refresh();
        m_deviceControlWidget->refreshStageDevices();
    }

    // Restore persistent settings that affect core behavior
    void MainWindow::applyStoredApplicationSettings()
    {
        constexpr qint64 kDefaultRecordedMaxBytes = 16ll * 1024 * 1024 * 1024;
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const qint64 recordedMaxBytes = settings
                                        .value(
                                            QStringLiteral("Recording/MaxPendingWriteBytes"), kDefaultRecordedMaxBytes)
                                        .toLongLong();
        m_scopeonecore->setRecordingMaxPendingWriteBytes(recordedMaxBytes);
    }

    // Record the application startup state in one place
    void MainWindow::logStartupSummary()
    {
        m_consoleWidget->addMessage(QStringLiteral("ScopeOne UI ready"), QStringLiteral("SYSTEM"));
        m_consoleWidget->addMessage(
            QStringLiteral("Qt runtime: %1").arg(QString::fromLatin1(qVersion())),
            QStringLiteral("SYSTEM"));
        m_consoleWidget->addMessage(
            QStringLiteral("Application directory: %1").arg(QCoreApplication::applicationDirPath()),
            QStringLiteral("SYSTEM"));
        showStatusMessage(tr("ScopeOne ready"), 3000);
    }

    // Show one transient status message without disturbing persistent fields
    void MainWindow::showStatusMessage(const QString& message, int timeoutMs)
    {
        const QString text = message.trimmed().isEmpty() ? tr("Ready") : message;
        setStatusLabelText(m_statusMessageLabel, text, text);
        if (timeoutMs > 0)
        {
            m_statusMessageTimer->start(timeoutMs);
            return;
        }
        m_statusMessageTimer->stop();
    }

    void MainWindow::setCursorStatus(const QString& text)
    {
        const QString statusText = text.trimmed().isEmpty() ? tr("Cursor: -") : text;
        setStatusLabelText(m_statusCursorLabel, statusText, statusText);
    }

    void MainWindow::clearCursorStatus()
    {
        const QString statusText = tr("Cursor: -");
        const QString tooltip = tr("Pixel readout");
        setStatusLabelText(m_statusCursorLabel, statusText, tooltip);
    }

    // Sample the last cursor position against the newest graph frame
    void MainWindow::refreshPreviewCursorStatus()
    {
        if (m_lastPreviewMousePos.x() < 0 || m_lastPreviewMousePos.y() < 0)
        {
            return;
        }

        PreviewWidget::PreviewInteractionTarget target;
        if (!m_previewWidget->interactionTargetAt(m_lastPreviewMousePos, target))
        {
            clearCursorStatus();
            return;
        }

        int value = 0;
        const bool valueOk = m_scopeonecore->graphPixelValue(target.layerKey, target.imagePos, value);
        const int layerKeySeparator = target.layerKey.indexOf(QLatin1Char(':'));
        const QString sourceKind = layerKeySeparator > 0
                                       ? target.layerKey.left(layerKeySeparator)
                                       : (target.processed ? QStringLiteral("proc") : QStringLiteral("raw"));
        const QString layerName = m_previewWidget->layerName(target.layerKey);
        const QString msg = valueOk
                                ? QString("Cursor: %1 [%2] x=%3 y=%4 value=%5")
                                  .arg(layerName, sourceKind)
                                  .arg(target.imagePos.x()).arg(target.imagePos.y())
                                  .arg(value)
                                : QString("Cursor: %1 [%2] x=%3 y=%4 value=-")
                                  .arg(layerName, sourceKind)
                                  .arg(target.imagePos.x()).arg(target.imagePos.y());
        setCursorStatus(msg);
    }

    // Edit persistent application settings
    void MainWindow::openSettingsDialog()
    {
        constexpr qint64 kDefaultRecordedMaxBytes = 16ll * 1024 * 1024 * 1024;
        const qint64 currentValue = m_scopeonecore->recordingMaxPendingWriteBytes();

        SettingsDialog dialog(currentValue > 0 ? currentValue : kDefaultRecordedMaxBytes, this);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        const qint64 recordedMaxBytes = dialog.maxPendingWriteBytes();
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        settings.setValue(QStringLiteral("Recording/MaxPendingWriteBytes"), recordedMaxBytes);
        m_scopeonecore->setRecordingMaxPendingWriteBytes(recordedMaxBytes);
        showStatusMessage(
            tr("Recording buffer limit updated to %1 bytes").arg(recordedMaxBytes),
            5000);
    }

    // Open the stage driven image mosaic tool
    void MainWindow::openStageMosaicTool()
    {
        if (!m_stageMosaicDialog)
        {
            auto* dialog = new StageMosaicDialog(m_scopeonecore, m_previewWidget, this);
            connect(dialog,
                    &StageMosaicDialog::gallerySessionCreated,
                    this,
                    [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                           const QString& title)
                    {
                        m_imageGalleryWidget->addSession(session, title);
                        m_scopeonecore->removeStaticFrame(QStringLiteral("stage_mosaic"));
                        previewGallerySession(*m_scopeonecore, *m_previewWidget, session);
                        showStatusMessage(tr("Mosaic added to Gallery"), 5000);
                    });
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setModal(false);
            m_stageMosaicDialog = dialog;
        }
        m_stageMosaicDialog->show();
        m_stageMosaicDialog->raise();
        m_stageMosaicDialog->activateWindow();
    }

    // Open the OpenCV particle detection tool
    void MainWindow::openParticleDetectionTool()
    {
        if (!m_particleDetectionDialog)
        {
            m_particleDetectionDialog = new ParticleDetectionDialog(m_scopeonecore, m_previewWidget, this);
            m_particleDetectionDialog->setAttribute(Qt::WA_DeleteOnClose);
            m_particleDetectionDialog->setModal(false);
        }
        m_particleDetectionDialog->show();
        m_particleDetectionDialog->raise();
        m_particleDetectionDialog->activateWindow();
    }

    // Display image coordinates and pixel value under the cursor
    void MainWindow::handlePreviewMousePosition(const QPoint& pos)
    {
        m_lastPreviewMousePos = pos;
        if (pos.x() < 0 || pos.y() < 0)
        {
            clearCursorStatus();
            return;
        }
        refreshPreviewCursorStatus();
    }

    // Apply a user drawn ROI through the core facade
    void MainWindow::handleRoiDrawn(const QString& cameraId,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    int sourceRoiX,
                                    int sourceRoiY)
    {
        x += sourceRoiX;
        y += sourceRoiY;

        // Backend restarts preview for ROI
        if (m_scopeonecore->setROI(cameraId, x, y, width, height))
        {
            m_scopeonecore->clearLiveFrames(cameraId);
            showStatusMessage(tr("ROI applied"), 3000);
            qInfo().noquote() << QString("ROI set for %1: %2x%3 at (%4,%5)")
                                 .arg(cameraId)
                                 .arg(width)
                                 .arg(height)
                                 .arg(x)
                                 .arg(y);
        }
        else
        {
            showStatusMessage(tr("Failed to set ROI"), 5000);
            qWarning().noquote() << QString("Failed to set ROI for %1").arg(cameraId);
        }
    }

    // Apply UI state after a configuration load attempt
    void MainWindow::handleConfigurationLoadFinished(bool success,
                                                     const QString& configPath,
                                                     const QStringList& cameraIds,
                                                     bool foundCamera,
                                                     int successCount,
                                                     int failCount,
                                                     int skippedCameraCount,
                                                     const QString& errorMessage)
    {
        closeLoadConfigProgress();

        if (!success)
        {
            QMessageBox::critical(this, tr("Load Failed"),
                                  tr("Failed to load configuration: %1").arg(errorMessage));
            showStatusMessage(tr("Configuration load failed"), 5000);
            qCritical().noquote() << QString("Configuration failed: %1").arg(errorMessage);
            return;
        }

        if (successCount > 0)
        {
            qInfo().noquote() << QString("%1 device(s) initialized successfully").arg(successCount);
        }
        if (failCount > 0)
        {
            showStatusMessage(tr("%1 device(s) failed to initialize").arg(failCount), 5000);
            qWarning().noquote() << QString("%1 device(s) failed to initialize").arg(failCount);
        }
        if (skippedCameraCount > 0)
        {
            qInfo().noquote() << QString("%1 camera device(s) skipped for UI initialization")
                .arg(skippedCameraCount);
        }

        if (!cameraIds.isEmpty())
        {
            applyLoadedCameraState(cameraIds);
            if (failCount > 0)
            {
                showStatusMessage(
                    tr("%1 camera(s) ready, %2 device warning(s)").arg(cameraIds.size()).arg(failCount),
                    5000);
            }
            else
            {
                showStatusMessage(tr("%1 camera(s) ready").arg(cameraIds.size()), 5000);
            }
            qInfo().noquote() << QString("%1 camera(s) ready").arg(cameraIds.size());
        }
        else if (foundCamera)
        {
            showStatusMessage(tr("No cameras were successfully initialized"), 5000);
            qInfo().noquote() << "No cameras were successfully initialized";
        }
        else
        {
            showStatusMessage(tr("No camera devices in configuration"), 5000);
            qWarning().noquote() << "No camera devices in configuration";
        }

        refreshDevicePanels(false);

        qInfo().noquote() << QString("Configuration loaded successfully: %1").arg(configPath);
    }

    // Apply UI state after a configuration unload attempt
    void MainWindow::handleConfigurationUnloadFinished(bool success,
                                                       const QStringList& cameraIds,
                                                       const QString& errorMessage)
    {
        if (!success)
        {
            QMessageBox::critical(this, tr("Unload Failed"),
                                  tr("Failed to unload configuration: %1").arg(errorMessage));
            showStatusMessage(tr("Configuration unload failed"), 5000);
            qCritical().noquote() << QString("Configuration unload failed: %1").arg(errorMessage);
            return;
        }

        applyUnloadedCameraState(cameraIds);
        refreshDevicePanels();
        qInfo().noquote() << "Configuration unload completed successfully";
    }

    // Load a Micro Manager config selected by the user
    void MainWindow::loadConfigurationFromDialog()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        QString lastConfigDir = settings.value(QStringLiteral("LastConfigDirectory"),
                                               QCoreApplication::applicationDirPath() + "/config").toString();
        if (!QDir(lastConfigDir).exists())
        {
            lastConfigDir = QCoreApplication::applicationDirPath() + "/config";
        }

        const QString fileName = QFileDialog::getOpenFileName(this,
                                                              tr("Load Configuration"),
                                                              lastConfigDir,
                                                              tr("Configuration Files (*.cfg);;All Files (*.*)"));
        if (fileName.isEmpty())
        {
            return;
        }

        settings.setValue(QStringLiteral("LastConfigDirectory"), QFileInfo(fileName).absolutePath());
        closeLoadConfigProgress();

        const QStringList previousCameraIds = m_scopeonecore->cameraIds();
        if (!m_scopeonecore->loadedDevices().isEmpty())
        {
            m_scopeonecore->unloadConfiguration();
            applyUnloadedCameraState(previousCameraIds);
            refreshDevicePanels();
        }

        m_loadConfigProgress = new QProgressDialog(tr("Loading configuration..."),
                                                   QString(),
                                                   0, 0,
                                                   this);
        m_loadConfigProgress->setWindowTitle(tr("Loading"));
        m_loadConfigProgress->setWindowModality(Qt::ApplicationModal);
        m_loadConfigProgress->setCancelButton(nullptr);
        m_loadConfigProgress->setMinimumDuration(0);
        m_loadConfigProgress->setMinimumWidth(520);
        m_loadConfigProgress->setAutoClose(false);
        m_loadConfigProgress->setAutoReset(false);
        m_loadConfigProgress->show();
        qApp->processEvents();

        showStatusMessage(tr("Loading configuration..."));
        qInfo().noquote() << QString("Loading configuration: %1").arg(fileName);
        scopeone::core::ScopeOneCore::LoadConfigResult result;
        QString errorMessage;
        const bool success = m_scopeonecore->loadConfiguration(fileName, &result, &errorMessage);
        handleConfigurationLoadFinished(success,
                                        fileName,
                                        result.cameraIds,
                                        result.foundCamera,
                                        result.successCount,
                                        result.failCount,
                                        result.skippedCameraCount,
                                        errorMessage);
    }

    // Confirm and unload the active device configuration
    void MainWindow::unloadConfigurationWithConfirmation()
    {
        const QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            tr("Unload Configuration"),
            tr("This will unload all devices. Continue?"),
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes)
        {
            const QStringList cameraIds = m_scopeonecore->cameraIds();
            m_scopeonecore->unloadConfiguration();
            handleConfigurationUnloadFinished(true, cameraIds, QString{});
        }
    }

    // Toggle the main window between fullscreen and normal modes
    void MainWindow::setFullScreenEnabled(bool enabled)
    {
        if (enabled)
        {
            showFullScreen();
            return;
        }
        showNormal();
    }
} // namespace scopeone::ui
