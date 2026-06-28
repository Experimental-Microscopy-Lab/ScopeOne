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
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QVector>
#include <memory>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        // Build the preview layer key for a raw camera feed
        QString rawLayerKey(const QString& cameraId)
        {
            return QStringLiteral("raw:%1").arg(cameraId);
        }

        // Build raw layer keys for all camera ids
        QStringList rawLayerKeys(const QStringList& cameraIds)
        {
            QStringList layerKeys;
            layerKeys.reserve(cameraIds.size());
            for (const QString& cameraId : cameraIds)
            {
                layerKeys.append(rawLayerKey(cameraId));
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
                if (layerKey.startsWith(QStringLiteral("raw:")))
                {
                    rawKeys.append(layerKey);
                }
            }
            return rawKeys;
        }

        // Convert stored recording data into a preview frame
        scopeone::core::ImageFrame normalizedRecordingFrame(
            const scopeone::core::ScopeOneCore::RecordingFrame& frame,
            const QString& cameraId)
        {
            scopeone::core::ImageFrame normalized;
            normalized.cameraId = cameraId;
            normalized.width = (frame.width > 0) ? frame.width : static_cast<int>(frame.header.width);
            normalized.height = (frame.height > 0) ? frame.height : static_cast<int>(frame.header.height);
            normalized.bitsPerSample = (frame.bits > 0) ? frame.bits : static_cast<int>(frame.header.bitsPerSample);
            if (normalized.bitsPerSample <= 0)
            {
                normalized.bitsPerSample = 8;
            }
            if (frame.header.pixelFormat == static_cast<quint32>(scopeone::core::SharedPixelFormat::Mono16))
            {
                normalized.pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
            }
            else if (frame.header.pixelFormat == static_cast<quint32>(scopeone::core::SharedPixelFormat::Mono8))
            {
                normalized.pixelFormat = scopeone::core::ImagePixelFormat::Mono8;
            }
            else
            {
                normalized.pixelFormat = (normalized.bitsPerSample > 8)
                                             ? scopeone::core::ImagePixelFormat::Mono16
                                             : scopeone::core::ImagePixelFormat::Mono8;
            }
            const int bytesPerPixel = normalized.bytesPerPixel();
            normalized.stride = (frame.header.stride > 0)
                                    ? static_cast<int>(frame.header.stride)
                                    : normalized.width * bytesPerPixel;
            normalized.bytes = frame.rawData;
            return normalized;
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

        // Build the preview layer key for one static layer id
        QString staticLayerKey(const QString& layerId)
        {
            return rawLayerKey(QStringLiteral("static:%1").arg(layerId));
        }

        // Build the preview layer key for one gallery session static layer
        QString gallerySessionPreviewLayerKey(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& cameraId)
        {
            return staticLayerKey(gallerySessionLayerId(session, cameraId));
        }

        // Check whether one preview layer key points to static data
        bool isStaticLayerKey(const QString& layerKey)
        {
            return layerKey.startsWith(staticLayerKey(QString()));
        }

        // Push the first frame of each recorded camera into the shared preview engine
        QStringList previewGallerySession(
            PreviewWidget& previewWidget,
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            QStringList selectedLayerKeys = previewWidget.selectedLayerKeys();
            QStringList openedLayerKeys;

            for (const QString& cameraId : session->recordedCameraIds())
            {
                const auto* frames = session->framesForCamera(cameraId);
                if (!frames || frames->empty())
                {
                    continue;
                }

                const scopeone::core::ImageFrame frame = normalizedRecordingFrame(frames->front(), cameraId);
                if (!frame.isValid())
                {
                    continue;
                }

                const QString displayName = frames->size() > 1
                                                ? QStringLiteral("Gallery %1 Frame 1").arg(cameraId)
                                                : QStringLiteral("Gallery %1").arg(cameraId);
                const QString layerKey = previewWidget.setStaticLayerFrame(
                    gallerySessionLayerId(session, cameraId),
                    displayName,
                    frame);
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

        // Remove preview layers that belong to one gallery session
        void removeGallerySessionPreview(
            PreviewWidget& previewWidget,
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            for (const QString& cameraId : session->recordedCameraIds())
            {
                previewWidget.removeStaticLayer(gallerySessionPreviewLayerKey(session, cameraId));
            }
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

        m_apiServer = new ScopeOneLocalApiServer(m_scopeonecore, this);
        setupUI();
        setupSignalWiring();
        applyStoredApplicationSettings();

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
                });

        connect(m_previewWidget, &PreviewWidget::mousePositionChanged,
                this, &MainWindow::handlePreviewMousePosition);
        connect(m_previewWidget, &PreviewWidget::roiDrawn,
                this, &MainWindow::handleRoiDrawn);
        connect(m_previewWidget, &PreviewWidget::lineDrawn,
                this, [this](const QString& layerKey,
                             const QString& cameraId,
                             int startX,
                             int startY,
                             int endX,
                             int endY,
                             bool processed)
                {
                    const QPoint start(startX, startY);
                    const QPoint end(endX, endY);
                    if (isStaticLayerKey(layerKey))
                    {
                        m_scopeonecore->clearLineProfile();
                        QVector<int> values;
                        if (m_previewWidget->lineProfile(cameraId, start, end, processed, values))
                        {
                            m_inspectWidget->setLayerCrossSectionProfile(layerKey, values);
                        }
                        return;
                    }

                    m_scopeonecore->setLineProfile(cameraId,
                                                   start,
                                                   end,
                                                   processed);
                });

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::newRawFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    if (!frame.isValid()
                        || !m_deviceControlWidget->acceptsCameraFrame(frame.cameraId))
                    {
                        return;
                    }
                    m_previewWidget->setRawFrame(frame);
                }, Qt::QueuedConnection);

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processedFrameReady,
                this, [this](const QString& cameraId, const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setProcessedFrame(cameraId, frame);
                });

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
                            statusBar()->showMessage(tr("No raw frame available for Half ROI"), 5000);
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
                        m_previewWidget->clearCameraFrames(cameraId);
                        qInfo().noquote() << QString("Half ROI set for %1: %2x%3 at (%4,%5)")
                                             .arg(cameraId)
                                             .arg(roiWidth)
                                             .arg(roiHeight)
                                             .arg(x)
                                             .arg(y);
                    }
                    else
                    {
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
                        const bool success = m_scopeonecore->clearROI(id);

                        if (success)
                        {
                            m_previewWidget->clearCameraFrames(id);
                            qInfo().noquote() << QString("ROI restored for %1").arg(id);
                        }
                        else
                        {
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
        connect(m_previewWidget, &PreviewWidget::staticLayerFrameChanged,
                this, [this](const QString& layerKey, const scopeone::core::ImageFrame& frame)
                {
                    scopeone::core::ScopeOneCore::HistogramStats stats;
                    if (scopeone::core::ScopeOneCore::computeHistogramStats(frame, stats))
                    {
                        m_inspectWidget->setLayerInspect(layerKey, stats);
                    }
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::exposureValueChanged,
                this, [this](double ms)
                {
                    if (!m_scopeonecore->setExposure(m_currentControlTarget, ms))
                    {
                        qWarning().noquote() << QString("Failed to set exposure: %1 ms").arg(ms);
                    }
                    m_deviceControlWidget->refreshCameraParameters();
                    m_propertyBrowser->refresh(false);
                });

        connect(m_inspectWidget, &InspectWidget::requestDrawCrossSectionLayer,
                this, [this](const QString& layerKey)
                {
                    m_previewWidget->startLineDrawingForLayer(layerKey);
                    statusBar()->showMessage(QStringLiteral("Drag a line on the preview"));
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
                    QStringList selectedLayerKeys = m_previewWidget->selectedLayerKeys();
                    const QStringList availableCameraIds = m_previewWidget->availableCameraIds();

                    for (const QString& layerKey : std::as_const(selectedLayerKeys))
                    {
                        if (!layerKey.startsWith(QStringLiteral("raw:")))
                        {
                            continue;
                        }
                        const QString cameraId = layerKey.mid(4);
                        if (cameraId.isEmpty() || !availableCameraIds.contains(cameraId))
                        {
                            continue;
                        }
                        const QString processedLayerKey = QStringLiteral("proc:%1").arg(cameraId);
                        if (!selectedLayerKeys.contains(processedLayerKey))
                        {
                            selectedLayerKeys.append(processedLayerKey);
                        }
                    }

                    if (selectedLayerKeys.isEmpty())
                    {
                        for (const QString& cameraId : availableCameraIds)
                        {
                            selectedLayerKeys.append(QStringLiteral("proc:%1").arg(cameraId));
                        }
                    }

                    m_previewWidget->setSelectedLayerKeys(selectedLayerKeys);
                    m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::SideBySide);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStopped,
                this, [this]()
                {
                    QStringList selectedLayerKeys = rawOnlyLayerKeys(m_previewWidget->selectedLayerKeys());
                    if (selectedLayerKeys.isEmpty())
                    {
                        selectedLayerKeys = rawLayerKeys(m_previewWidget->availableCameraIds());
                    }
                    m_previewWidget->setSelectedLayerKeys(selectedLayerKeys);
                    m_previewWidget->clearProcessedFrames();
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
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionSaveFinished,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    if (session && session->isSaved())
                    {
                        m_imageGalleryWidget->markSessionSaved(session);
                    }
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionOpenRequested,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    const QStringList selectedLayerKeys = previewGallerySession(*m_previewWidget, session);
                    if (selectedLayerKeys.isEmpty())
                    {
                        statusBar()->showMessage(tr("No gallery image available for preview"), 5000);
                        return;
                    }
                    statusBar()->showMessage(
                        tr("Gallery preview opened with %1 layer(s)").arg(selectedLayerKeys.size()),
                        5000);
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::livePreviewRequested,
                this, &MainWindow::showLivePreview);
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionRemoved,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    removeGallerySessionPreview(*m_previewWidget, session);
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
                    qInfo().noquote() << QString("Property changed: %1.%2 = %3")
                        .arg(device, property, value);
                    m_deviceControlWidget->refreshCameraParameters();
                });
        connect(m_propertyBrowser, &DevicePropertyWidget::errorOccurred,
                this, [](const QString& message)
                {
                    qCritical().noquote() << message;
                });
        connect(m_configPresetWidget, &ConfigPresetWidget::configChanged,
                this, [](const QString& group, const QString& preset)
                {
                    qInfo().noquote() << QString("Config changed: %1 = %2").arg(group, preset);
                });
        connect(m_configPresetWidget, &ConfigPresetWidget::errorOccurred,
                this, [](const QString& message)
                {
                    qCritical().noquote() << message;
                });
    }

    // Create the preview area and all docked tool panels
    void MainWindow::setupUI()
    {
        m_previewWidget = new PreviewWidget(this);
        setCentralWidget(m_previewWidget);

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
            statusBar()->showMessage(tr("No live camera available"), 5000);
            return;
        }

        m_previewWidget->clearStaticLayers();

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
            m_previewWidget->setSelectedLayerKeys({rawLayerKey(m_currentControlTarget)});
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
            m_previewWidget->clearCameraFrames(id);
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

        if (cameraIds.size() > 1)
        {
            m_previewWidget->setSelectedLayerKeys(rawLayerKeys(cameraIds));
        }
        else if (!cameraIds.isEmpty())
        {
            m_previewWidget->setSelectedLayerKeys({rawLayerKey(cameraIds.first())});
        }

        m_recordingWidget->setAvailableCameras(cameraIds);
    }

    // Clear preview and panel state after devices unload
    void MainWindow::applyUnloadedCameraState(const QStringList& cameraIds)
    {
        for (const QString& id : cameraIds)
        {
            m_previewWidget->clearCameraFrames(id);
        }
        m_previewWidget->setAvailableCameraIds({});
        m_previewWidget->clearLine();
        m_deviceControlWidget->setControlTargets({});
        m_deviceControlWidget->onCameraInitialized(false);
        m_inspectWidget->setAvailableCameras({});
        m_inspectWidget->onCameraInitialized(false);
        m_inspectWidget->clearCrossSectionProfile();

        m_recordingWidget->setAvailableCameras({});
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
        statusBar()->showMessage(
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
                        m_previewWidget->removeStaticLayer(staticLayerKey(QStringLiteral("stage_mosaic")));
                        previewGallerySession(*m_previewWidget, session);
                        statusBar()->showMessage(tr("Mosaic added to Gallery"), 5000);
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
        if (pos.x() < 0 || pos.y() < 0)
        {
            statusBar()->clearMessage();
            return;
        }

        PreviewWidget::PreviewInteractionTarget target;
        if (!m_previewWidget->interactionTargetAt(pos, target))
        {
            statusBar()->showMessage(QString("Pos: (%1,%2) | Value: -").arg(pos.x()).arg(pos.y()));
            return;
        }

        int value = 0;
        const bool valueOk = m_previewWidget->getPixelValue(target.cameraId,
                                                            target.imagePos,
                                                            target.processed,
                                                            value);
        const QString sourceKind = target.processed ? QStringLiteral("proc") : QStringLiteral("raw");
        const QString layerName = m_previewWidget->layerName(target.layerKey);
        const QString msg = valueOk
                                ? QString("%1 [%2] Pos: (%3,%4) | Value: %5")
                                  .arg(layerName, sourceKind)
                                  .arg(target.imagePos.x()).arg(target.imagePos.y())
                                  .arg(value)
                                : QString("%1 [%2] Pos: (%3,%4) | Value: -")
                                  .arg(layerName, sourceKind)
                                  .arg(target.imagePos.x()).arg(target.imagePos.y());
        statusBar()->showMessage(msg);
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
        const bool success = m_scopeonecore->setROI(cameraId, x, y, width, height);
        if (success)
        {
            m_previewWidget->clearCameraFrames(cameraId);
            qInfo().noquote() << QString("ROI set for %1: %2x%3 at (%4,%5)")
                                 .arg(cameraId)
                                 .arg(width)
                                 .arg(height)
                                 .arg(x)
                                 .arg(y);
        }
        else
        {
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
            qCritical().noquote() << QString("Configuration failed: %1").arg(errorMessage);
            return;
        }

        if (successCount > 0)
        {
            qInfo().noquote() << QString("%1 device(s) initialized successfully").arg(successCount);
        }
        if (failCount > 0)
        {
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
            qInfo().noquote() << QString("%1 camera(s) ready").arg(cameraIds.size());
        }
        else if (foundCamera)
        {
            qInfo().noquote() << "No cameras were successfully initialized";
        }
        else
        {
            qWarning().noquote() << "No camera devices in configuration";
        }

        refreshDevicePanels(false);

        qInfo().noquote() << "Property browser refreshed";
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
