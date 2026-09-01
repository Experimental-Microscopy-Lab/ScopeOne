#include "MainWindow.h"

#include "scopeone/ScopeOneCore.h"
#include "AboutDialog.h"
#include "InspectWidget.h"
#include "ConsoleWidget.h"
#include "DeviceControlWidget.h"
#include "DevicePropertyWidget.h"
#include "ConfigPresetWidget.h"
#include "PreviewWidget.h"
#include "scopeone/ImageSceneModel.h"
#include "ImageGalleryWidget.h"
#include "ImageWorkspace.h"
#include "ImageToolsDialog.h"
#include "ImageProcessingWidget.h"
#include "RecordingWidget.h"
#include "SettingsDialog.h"
#include "PluginManagerDialog.h"
#include "ScopeOneLocalApiServer.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QStyleHints>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <QUrl>
#include <QVector>
#include <cmath>
#include <memory>
#include <utility>

namespace scopeone::ui
{
    using ImageSceneModel = scopeone::core::ImageSceneModel;

    namespace
    {
        constexpr qsizetype kMaxRecentConfigurations = 8;

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

        // Detect the standard 64 bit Micro-Manager installation
        QString detectedMicroManagerDirectory()
        {
#ifdef Q_OS_WIN
            const QString programFiles = qEnvironmentVariable("ProgramFiles");
            if (programFiles.isEmpty())
            {
                return {};
            }
            const QString directoryPath = QDir(programFiles).filePath(QStringLiteral("Micro-Manager-2.0"));
            const QDir directory(directoryPath);
            if (directory.exists()
                && !directory.entryList({QStringLiteral("mmgr_dal_*.dll")}, QDir::Files).isEmpty())
            {
                return QDir::cleanPath(directory.absolutePath());
            }
#endif
            return {};
        }

        // Apply the requested Qt color scheme
        void applyColorScheme(const QString& colorScheme)
        {
            QStyleHints* styleHints = QApplication::styleHints();
            if (colorScheme == QStringLiteral("light"))
            {
                styleHints->setColorScheme(Qt::ColorScheme::Light);
            }
            else if (colorScheme == QStringLiteral("dark"))
            {
                styleHints->setColorScheme(Qt::ColorScheme::Dark);
            }
            else
            {
                styleHints->unsetColorScheme();
            }
        }

        void updateSessionPresentation(
            scopeone::core::ScopeOneCore& core,
            const scopeone::core::ImageSceneModel& sceneModel,
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            if (!session)
            {
                return;
            }

            QString errorMessage;
            if (!core.setRecordingSessionPresentation(session, sceneModel.document(), &errorMessage))
            {
                qWarning().noquote() << QStringLiteral("Failed to persist image presentation: %1")
                                          .arg(errorMessage);
            }
        }
    }

    // Build the main shell around the shared core facade
    MainWindow::MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QMainWindow(parent)
          , m_imageSceneModel(core ? core->imageSceneModel() : nullptr)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("MainWindow requires ScopeOneCore");
        }

        applyStoredApplicationSettings();
        m_imageWorkspace = new ImageWorkspace(core, this, this);
        setupUI();
        m_imageWorkspace->setLiveViewer(m_previewWidget);
        setupSignalWiring();
        new ScopeOneLocalApiServer(m_scopeonecore, m_previewWidget, m_imageWorkspace, this);
        logStartupSummary();

        setWindowTitle("ScopeOne");
        setMinimumSize(1366, 768);
        resize(1600, 900);
    }

    // Confirm what to do with unsaved gallery sessions
    void MainWindow::closeEvent(QCloseEvent* event)
    {
        if (m_closeSaveInProgress)
        {
            event->ignore();
            return;
        }

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
            event->ignore();
            m_closePendingSessions = unsavedSessions;
            m_closeSaveTotal = unsavedSessions.size();
            m_closeSaveInProgress = true;
            m_closeAfterSave = true;
            m_closeSaveProgress = new QProgressDialog(
                tr("Saving gallery images..."), tr("Keep Open"), 0, m_closeSaveTotal, this);
            m_closeSaveProgress->setWindowTitle(tr("Saving"));
            m_closeSaveProgress->setWindowModality(Qt::WindowModal);
            m_closeSaveProgress->setMinimumDuration(0);
            m_closeSaveProgress->setAutoClose(false);
            m_closeSaveProgress->setValue(0);
            connect(m_closeSaveProgress, &QProgressDialog::canceled, this, [this]()
            {
                m_closeAfterSave = false;
                m_closeSaveProgress->deleteLater();
                m_closeSaveProgress = nullptr;
            });
            m_closeSaveProgress->show();
            for (const auto& session : unsavedSessions)
            {
                updateSessionPresentation(*m_scopeonecore, *m_imageSceneModel, session);
                m_scopeonecore->saveRecordingSession(session);
            }
            return;
        }

        QMainWindow::closeEvent(event);
    }

    // Connect core events and panel actions into one UI flow
    void MainWindow::setupSignalWiring()
    {
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::hardwareConfigurationChanged,
                this, [this]()
                {
                    const bool configurationRunning =
                        m_scopeonecore->configurationOperationRunning();
                    m_loadConfigurationAction->setEnabled(!configurationRunning);
                    m_unloadConfigurationAction->setEnabled(!configurationRunning);
                    m_recentConfigurationsMenu->setEnabled(
                        !configurationRunning && !m_recentConfigurationsMenu->isEmpty());
                    m_propertyBrowser->setEnabled(!configurationRunning);
                    m_configPresetWidget->setEnabled(!configurationRunning);
                    m_deviceControlWidget->setControlsEnabled(!configurationRunning);
                    m_toolRegistry->setEnabled(!configurationRunning);

                    syncCameraState();
                    if (!configurationRunning)
                    {
                        refreshDevicePanels(false);
                        const QStringList cameraIds = m_scopeonecore->cameraIds();
                        if (m_scopeonecore->loadedConfigurationPath().isEmpty())
                        {
                            showStatusMessage(tr("Configuration unloaded"), 3000);
                        }
                        else if (cameraIds.isEmpty())
                        {
                            showStatusMessage(tr("Configuration loaded without cameras"), 5000);
                        }
                        else
                        {
                            showStatusMessage(tr("%1 camera(s) ready").arg(cameraIds.size()), 5000);
                        }
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::hardwareDevicesChanged,
                this, [this]()
                {
                    if (m_scopeonecore->configurationOperationRunning()) return;
                    syncCameraState();
                    refreshDevicePanels(false);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::configurationLoadFinished,
                this,
                [this](bool success,
                       const scopeone::core::ScopeOneCore::LoadConfigResult& result,
                       const QString& errorMessage)
                {
                    handleConfigurationLoadFinished(success,
                                                    m_scopeonecore->loadedConfigurationPath(),
                                                    result.cameraIds,
                                                    result.foundCamera,
                                                    result.successCount,
                                                    result.failCount,
                                                    result.skippedCameraCount,
                                                    errorMessage);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::configurationUnloadFinished,
                this, &MainWindow::handleConfigurationUnloadFinished);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::deviceStateChanged,
                this, [this]()
                {
                    if (!m_scopeonecore->configurationOperationRunning())
                    {
                        refreshDevicePanels(false);
                    }
                },
                Qt::QueuedConnection);

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewStateChanged,
                this, [this](bool running)
                {
                    m_previewRunning = running;
                    m_previewWidget->resetLiveFrameRates();
                    m_deviceControlWidget->setPreviewRunning(running);
                    m_deviceControlWidget->setControlTargetEnabled(!running);
                    setStatusLabelText(m_statusPreviewLabel,
                                       running ? tr("Preview: Live") : tr("Preview: Idle"),
                                       running ? tr("Preview is running") : tr("Preview is idle"));
                    showStatusMessage(running ? tr("Live preview started") : tr("Live preview stopped"), 3000);
                });

        connect(m_imageWorkspace, &ImageWorkspace::mousePositionChanged,
                this, &MainWindow::handlePreviewMousePosition);
        connect(m_previewWidget, &PreviewWidget::roiDrawn,
                this, &MainWindow::handleRoiDrawn);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::rawFramesAcquired,
                m_previewWidget, &PreviewWidget::trackRawFrameRate);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processedFramesCompleted,
                m_previewWidget, &PreviewWidget::trackProcessedFrameRate);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewRawFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    if (!frame.isValid())
                    {
                        return;
                    }
                    m_previewWidget->setGraphRawFrame(frame);
                    schedulePreviewCursorStatusRefresh();
                });

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewProcessedFrameReady,
                this, [this](const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setGraphProcessedFrame(frame);
                    schedulePreviewCursorStatusRefresh();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFramePublished,
                this, [this](const QString& sourceId,
                             const QString&,
                             const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setGraphStaticLayerFrame(sourceId, frame);
                    schedulePreviewCursorStatusRefresh();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFrameRemoved,
                this, [this](const QString& sourceId)
                {
                    const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(sourceId);
                    m_previewWidget->removeStaticLayer(layerKey);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::toolStreamFramePublished,
                this, [this](const QString& sourceId,
                             const QString&,
                             const scopeone::core::ImageFrame& frame)
                {
                    m_previewWidget->setGraphToolLayerFrame(sourceId, frame);
                    schedulePreviewCursorStatusRefresh();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFramesCleared,
                this, [this]()
                {
                    m_previewWidget->clearStaticLayers();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::liveFramesCleared,
                this, [this](const QString& sourceId)
                {
                    m_previewWidget->clearSourceFrames(sourceId);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processedFramesCleared,
                this, [this]()
                {
                    m_previewWidget->clearProcessedFrames();
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::startPreviewRequested,
                this, [this]()
                {
                    if (!m_scopeonecore->startPreview(m_currentControlTarget))
                    {
                        showStatusMessage(tr("Failed to start preview"), 5000);
                        qWarning().noquote() << QStringLiteral("Failed to start preview for %1")
                                                  .arg(m_currentControlTarget);
                    }
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::stopPreviewRequested,
                this, [this]()
                {
                    if (!m_scopeonecore->stopPreview(m_currentControlTarget))
                    {
                        showStatusMessage(tr("Failed to stop preview"), 5000);
                        qWarning().noquote() << QStringLiteral("Failed to stop preview for %1")
                                                  .arg(m_currentControlTarget);
                    }
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::requestDrawROI,
                this, [this](const QString& cameraId)
                {
                    m_previewWidget->startROIDrawing(cameraId);
                });

        connect(m_deviceControlWidget, &DeviceControlWidget::requestHalfROI,
                this, [this](const QString& cameraId)
                {
                    if (m_scopeonecore->setHalfROI(cameraId))
                    {
                        int x = 0;
                        int y = 0;
                        int width = 0;
                        int height = 0;
                        m_scopeonecore->getROI(cameraId, x, y, width, height);
                        qInfo().noquote() << QString("Half ROI set for %1: %2x%3 at (%4,%5)")
                                             .arg(cameraId).arg(width).arg(height).arg(x).arg(y);
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
                    if (m_scopeonecore->clearROI(target))
                    {
                        qInfo().noquote() << QString("ROI restored for %1").arg(target);
                    }
                    else
                    {
                        showStatusMessage(tr("Failed to restore ROI"), 5000);
                        qWarning().noquote() << QString("Failed to restore ROI for %1").arg(target);
                    }
                });

        connect(m_previewWidget, &PreviewWidget::stageStepRequested,
                m_deviceControlWidget, &DeviceControlWidget::moveXYStep);
        connect(m_previewWidget, &PreviewWidget::stageZStepRequested,
                m_deviceControlWidget, &DeviceControlWidget::moveZStep);

        connect(m_deviceControlWidget, &DeviceControlWidget::controlTargetChanged,
                this, &MainWindow::updateControlTarget);
        connect(m_deviceControlWidget, &DeviceControlWidget::exposureValueChanged,
                this, [this](double ms)
                {
                    if (!m_scopeonecore->setExposure(m_currentControlTarget, ms))
                    {
                        showStatusMessage(tr("Failed to set exposure"), 5000);
                        qWarning().noquote() << QString("Failed to set exposure: %1 ms").arg(ms);
                        m_deviceControlWidget->refreshCameraParameters();
                    }
                });

        connect(m_inspectWidget, &InspectWidget::requestDrawCrossSectionLayer,
                this, [this](const QString& layerKey)
                {
                    m_imageWorkspace->activePreviewWidget()->startCrossSectionDrawingForLayer(layerKey);
                    showStatusMessage(tr("Drag a line on the preview"), 5000);
                });

        connect(m_inspectWidget, &InspectWidget::requestClearCrossSection,
                this, [this]()
                {
                    m_imageWorkspace->activePreviewWidget()->clearCrossSection();
                });

        connect(m_inspectWidget, &InspectWidget::requestDrawMeasurementLine,
                this, [this](const QString& layerKey)
                {
                    m_imageWorkspace->activePreviewWidget()->startMeasurementLineDrawingForLayer(layerKey);
                    showStatusMessage(tr("Drag a line on the preview"), 5000);
                });
        connect(m_inspectWidget, &InspectWidget::requestClearMeasurementLines,
                this, [this](const QString& layerKey)
                {
                    m_imageWorkspace->activeSceneModel()->clearRole(
                        ImageSceneModel::MarkupRole::Measurement, layerKey);
                    m_inspectWidget->clearMeasurementLine();
                });
        connect(m_imageWorkspace, &ImageWorkspace::measurementLineDrawn,
                this, [this](const QString& layerKey, const QPoint& start, const QPoint& end)
                {
                    ImageSceneModel* sceneModel = m_imageWorkspace->activeSceneModel();
                    const QString markupId = sceneModel->createLine(
                        layerKey,
                        start,
                        end,
                        QString(),
                        ImageSceneModel::MarkupRole::Measurement);
                    sceneModel->selectOnly(markupId);
                    showMeasurementLine(layerKey, start, end);
                });
        connect(m_imageWorkspace, &ImageWorkspace::measurementLineInspected,
                this, [this](const QString& layerKey,
                             const QPoint& start,
                             const QPoint& end)
                {
                    showMeasurementLine(layerKey, start, end);
                });
        connect(m_imageWorkspace, &ImageWorkspace::measurementLineCleared,
                m_inspectWidget, &InspectWidget::clearMeasurementLine);

        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStarted,
                this, [this]()
                {
                    m_imageWorkspace->activateLiveViewer();
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Live"),
                                       tr("Processing is running"));
                    showStatusMessage(tr("Image processing started"), 3000);
                    const QStringList availableCameraIds = m_previewWidget->availableCameraIds();
                    QStringList visibleLayerKeys;
                    for (const QString& cameraId : availableCameraIds)
                    {
                        visibleLayerKeys.append(scopeone::core::ScopeOneCore::rawLayerKey(cameraId));
                        visibleLayerKeys.append(scopeone::core::ScopeOneCore::processedLayerKey(cameraId));
                    }

                    m_imageWorkspace->setVisibleLayers(visibleLayerKeys, true);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStopped,
                this, [this]()
                {
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Off"),
                                       tr("Processing is off"));
                    showStatusMessage(tr("Image processing stopped"), 3000);
                    const QStringList visibleLayerKeys =
                        rawLayerKeys(m_previewWidget->availableCameraIds());
                    m_imageWorkspace->setVisibleLayers(visibleLayerKeys);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processedLayerReady,
                this, [this](const QString& layerKey)
                {
                    showLayers({layerKey});
                    showStatusMessage(tr("Processed image added to preview"), 5000);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processedStackReady,
                this, [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_imageGalleryWidget->addSession(session, tr("Processed Stack"));
                    m_imageWorkspace->openSession(session, tr("Processed Stack"));
                });

        connect(m_imageWorkspace, &ImageWorkspace::sessionAvailable,
                m_imageGalleryWidget, &ImageGalleryWidget::addSession);
        connect(m_imageWorkspace, &ImageWorkspace::activeViewerChanged,
                this, [this]()
                {
                    const bool liveViewer = m_imageWorkspace->isLiveViewerActive();
                    m_deviceControlWidget->setPreviewWidget(
                        m_imageWorkspace->activePreviewWidget());
                    m_deviceControlWidget->setViewerContext(liveViewer);
                });
        connect(m_imageWorkspace, &ImageWorkspace::activeDocumentChanged,
                this, [this](const QString& documentId)
                {
                    m_saveImageAsAction->setEnabled(!documentId.isEmpty());
                });
        connect(m_imageWorkspace, &ImageWorkspace::documentSaveFinished,
                this, [this](const QString&, bool success, const QString& message)
                {
                    showStatusMessage(message, success ? 5000 : 8000);
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
        connect(m_settingsAction, &QAction::triggered,
                this, &MainWindow::openSettingsDialog);

        connect(m_recordingWidget, &RecordingWidget::gallerySessionCaptured,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    updateSessionPresentation(*m_scopeonecore, *m_imageSceneModel, session);
                    m_imageGalleryWidget->addSession(session);
                    m_imageGalleryDockWidget->show();
                    m_imageGalleryDockWidget->raise();
                });
        connect(m_deviceControlWidget, &DeviceControlWidget::snapRequested,
                this, [this](const QString& target)
                {
                    if (!m_recordingWidget->snapToGallery(target))
                    {
                        showStatusMessage(tr("No current frame is available to capture"), 5000);
                    }
                    else
                    {
                        showStatusMessage(tr("Snapshot captured"), 3000);
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_imageGalleryWidget->addSession(session);
                    m_imageGalleryDockWidget->show();
                    m_imageGalleryDockWidget->raise();
                    QTimer::singleShot(0, m_deviceControlWidget,
                                       [this]() { m_deviceControlWidget->refreshCameraParameters(); });
                    const QString result = session
                                               ? session->saveMessage()
                                               : QStringLiteral("Error: no session data");
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
                    handleCloseSaveFinished(session);
                    if (session && session->isSaved())
                    {
                        m_imageGalleryWidget->markSessionSaved(session);
                    }
                    const QString result = session
                                               ? session->saveMessage()
                                               : QStringLiteral("Error: no session data");
                    if (!result.isEmpty())
                    {
                        showStatusMessage(result, session && session->isSaved() ? 5000 : 8000);
                    }
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionOpenRequested,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    if (!session || !session->hasRecordedOutput())
                    {
                        showStatusMessage(tr("No gallery image available for preview"), 5000);
                        return;
                    }
                    m_imageWorkspace->openSession(session);
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::livePreviewRequested,
                this, &MainWindow::showLivePreview);
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionRemoved,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_scopeonecore->closeRecordingSession(
                        session->capturePlan().experimentId);
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::saveSessionsRequested,
                this,
                [this](const QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>>& sessions)
                {
                    for (const auto& session : sessions)
                    {
                        if (session)
                        {
                            updateSessionPresentation(*m_scopeonecore, *m_imageSceneModel, session);
                            m_scopeonecore->saveRecordingSession(session);
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
        m_previewWidget = new PreviewWidget(m_imageSceneModel, this);
        m_previewWidget->setPixelSizeCallback([this](const QString& layerKey)
        {
            return m_imageWorkspace->pixelSizeUm(layerKey);
        });
        setCentralWidget(m_imageWorkspace->viewerHost());
        setupTools();

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

    // Register built in tools and discover external tool plugins
    void MainWindow::setupTools()
    {
        m_toolRegistry = std::make_unique<ToolRegistry>(*this);
        m_toolRegistry->registerTool(
            {QStringLiteral("scopeone.scale"), tr("&Scale..."), {}, ToolWindowMode::Modal, true},
            [](ScopeOneToolContext& context, QWidget* parent)
            {
                return new CameraScaleDialog(&context.core(), parent);
            });
        m_toolRegistry->registerTool(
            {QStringLiteral("scopeone.stage_mosaic"), tr("Stage &Mosaic..."), {},
             ToolWindowMode::ModelessSingleton},
            [](ScopeOneToolContext& context, QWidget* parent)
            {
                return new StageMosaicDialog(context, parent);
            });
        m_toolRegistry->registerTool(
            {QStringLiteral("scopeone.particle_detection"), tr("&Particle Detection..."), {},
             ToolWindowMode::ModelessSingleton},
            [](ScopeOneToolContext& context, QWidget* parent)
            {
                return new ParticleDetectionDialog(context, parent);
            });

        const QString pluginDirectory = QDir(QCoreApplication::applicationDirPath())
                                            .filePath(QStringLiteral("plugins/tools"));
        for (const QString& error : m_toolRegistry->loadPlugins(pluginDirectory))
        {
            qWarning().noquote() << QStringLiteral("Failed to load tool plugin %1").arg(error);
        }
        const QString userPluginDirectory =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("plugins/tools"));
        for (const QString& error : m_toolRegistry->loadPlugins(userPluginDirectory))
        {
            qWarning().noquote() << QStringLiteral("Failed to load tool plugin %1").arg(error);
        }
    }

    scopeone::core::ScopeOneCore& MainWindow::core() const
    {
        return *m_scopeonecore;
    }

    QString MainWindow::currentLayerKey() const
    {
        return m_imageWorkspace->activeLayerKey();
    }

    scopeone::core::ImageFrame MainWindow::currentFrame() const
    {
        return m_scopeonecore->graphFrame(currentLayerKey());
    }

    scopeone::core::ImageFrame MainWindow::publishToolStreamFrame(
        const QString& sourceId,
        const scopeone::core::ImageFrame& frame,
        const QString& displayName)
    {
        return m_scopeonecore->publishToolStreamFrame(sourceId, frame, displayName);
    }

    void MainWindow::showLayers(const QStringList& layerKeys, bool sideBySide)
    {
        auto* activeScene = m_imageWorkspace->activeSceneModel();
        const bool belongsToLiveScene = std::all_of(
            layerKeys.cbegin(), layerKeys.cend(), [this](const QString& layerKey)
            {
                return m_imageSceneModel->layerIds().contains(layerKey);
            });
        if (belongsToLiveScene && activeScene != m_imageSceneModel)
        {
            m_imageWorkspace->activateLiveViewer();
        }
        m_imageWorkspace->setVisibleLayers(layerKeys, sideBySide);
    }

    void MainWindow::showToolStatus(const QString& message, int timeoutMs)
    {
        showStatusMessage(message, timeoutMs);
    }

    void MainWindow::presentSession(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
        const QString& title)
    {
        if (!session)
        {
            return;
        }
        m_imageGalleryWidget->addSession(session, title);
        m_imageWorkspace->openSession(session, title);
        showStatusMessage(tr("Images added to Gallery"), 5000);
    }

    // Create the shared status strip for transient and persistent state
    void MainWindow::setupStatusBar()
    {
        auto* bar = statusBar();
        bar->setSizeGripEnabled(false);

        m_statusMessageLabel = new QLabel(tr("Ready"), this);
        configureStatusLabel(m_statusMessageLabel, 260, 0, tr("Latest operation message"));

        m_statusCursorLabel = new QLabel(tr("x=- y=- value=-"), this);
        configureStatusLabel(m_statusCursorLabel, 260, 260, tr("Pixel readout"));

        m_statusPreviewLabel = new QLabel(tr("Preview: Idle"), this);
        configureStatusLabel(m_statusPreviewLabel, 100, 130, tr("Preview state"));

        m_statusProcessingLabel = new QLabel(tr("Processing: Off"), this);
        configureStatusLabel(m_statusProcessingLabel, 120, 150, tr("Processing state"));

        m_statusRecordingLabel = new QLabel(tr("Recording: Idle"), this);
        configureStatusLabel(m_statusRecordingLabel, 120, 150, tr("Recording state"));

        bar->addWidget(m_statusMessageLabel, 1);
        bar->addPermanentWidget(m_statusCursorLabel);
        bar->addPermanentWidget(m_statusPreviewLabel);
        bar->addPermanentWidget(m_statusProcessingLabel);
        bar->addPermanentWidget(m_statusRecordingLabel);

        m_statusMessageTimer = new QTimer(this);
        m_statusMessageTimer->setSingleShot(true);
        connect(m_statusMessageTimer, &QTimer::timeout, this, [this]()
        {
            setStatusLabelText(m_statusMessageLabel, tr("Ready"), tr("Latest operation message"));
        });

        m_cursorRefreshTimer = new QTimer(this);
        m_cursorRefreshTimer->setInterval(50);
        m_cursorRefreshTimer->setSingleShot(true);
        connect(m_cursorRefreshTimer, &QTimer::timeout,
                this, &MainWindow::refreshPreviewCursorStatus);
    }

    // Create application menus and persistent actions
    void MainWindow::setupMenuBar()
    {
        m_fileMenu = menuBar()->addMenu(tr("&File"));
        m_loadConfigurationAction = m_fileMenu->addAction(tr("&Load Configuration..."));
        m_recentConfigurationsMenu = m_fileMenu->addMenu(tr("&Recent Configurations"));
        connect(m_recentConfigurationsMenu, &QMenu::aboutToShow,
                this, &MainWindow::refreshRecentConfigurationsMenu);
        refreshRecentConfigurationsMenu();
        m_unloadConfigurationAction = m_fileMenu->addAction(tr("&Unload Configuration"));
        m_fileMenu->addSeparator();
        m_saveImageAsAction = m_fileMenu->addAction(tr("Save Image &As..."));
        m_saveImageAsAction->setEnabled(false);
        connect(m_saveImageAsAction, &QAction::triggered,
                m_imageWorkspace, [this]() { m_imageWorkspace->saveDocumentAs(); });
        m_fileMenu->addSeparator();
        m_exitAction = m_fileMenu->addAction(tr("E&xit"));

        m_viewMenu = menuBar()->addMenu(tr("&View"));
        m_fullScreenAction = m_viewMenu->addAction(tr("&Full Screen"));
        m_fullScreenAction->setCheckable(true);
        m_fullScreenAction->setShortcut(QKeySequence::FullScreen);

        m_fitToWindowAction = m_viewMenu->addAction(tr("Fit to &Window"));
        m_fitToWindowAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
        connect(m_fitToWindowAction, &QAction::triggered, this, [this]()
        {
            m_previewWidget->setFitToWindow(true);
        });

        m_actualSizeAction = m_viewMenu->addAction(tr("&Actual Size (100%)"));
        m_actualSizeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
        connect(m_actualSizeAction, &QAction::triggered, this, [this]()
        {
            m_previewWidget->setFitToWindow(false);
            m_previewWidget->setZoomPercent(100);
        });

        m_zoomInAction = m_viewMenu->addAction(tr("Zoom &In"));
        m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+=")), QKeySequence(QStringLiteral("Ctrl++"))});
        connect(m_zoomInAction, &QAction::triggered, this, [this]()
        {
            m_previewWidget->setFitToWindow(false);
            m_previewWidget->setZoomPercent(m_previewWidget->zoomPercent() + 20);
        });

        m_zoomOutAction = m_viewMenu->addAction(tr("Zoom &Out"));
        m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
        connect(m_zoomOutAction, &QAction::triggered, this, [this]()
        {
            m_previewWidget->setFitToWindow(false);
            m_previewWidget->setZoomPercent(m_previewWidget->zoomPercent() - 20);
        });

        m_viewMenu->addSeparator();

        m_scaleBarAction = m_viewMenu->addAction(tr("Show &Scale Bar"));
        m_scaleBarAction->setCheckable(true);
        m_scaleBarAction->setChecked(m_previewWidget->isScaleBarVisible());
        connect(m_scaleBarAction, &QAction::toggled, m_previewWidget, &PreviewWidget::setScaleBarVisible);
        connect(m_previewWidget, &PreviewWidget::scaleBarVisibilityChanged, m_scaleBarAction, &QAction::setChecked);

        m_clippingAction = m_viewMenu->addAction(tr("Show &Saturation Warning (Hi-Lo)"));
        m_clippingAction->setCheckable(true);
        m_clippingAction->setShortcut(QKeySequence(Qt::Key_C));
        m_clippingAction->setChecked(m_previewWidget->isClippingWarningEnabled());
        connect(m_clippingAction, &QAction::toggled, m_previewWidget, &PreviewWidget::setClippingWarningEnabled);
        connect(m_previewWidget, &PreviewWidget::clippingWarningChanged, m_clippingAction, &QAction::setChecked);

        m_toggleLayoutAction = m_viewMenu->addAction(tr("Toggle &Grid / Overlay Layout"));
        m_toggleLayoutAction->setShortcut(QKeySequence(Qt::Key_G));
        m_toggleLayoutAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(m_toggleLayoutAction, &QAction::triggered, this, [this]()
        {
            QWidget* focus = focusWidget();
            if (focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus)))
            {
                return;
            }
            if (m_previewWidget->layerLayoutMode() == PreviewWidget::LayerLayoutMode::SideBySide)
            {
                m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
                showStatusMessage(tr("Layout: Overlay Blended"), 2000);
            }
            else
            {
                m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::SideBySide);
                showStatusMessage(tr("Layout: Grid Split View"), 2000);
            }
        });
        addAction(m_toggleLayoutAction);

        m_viewMenu->addSeparator();
        m_dockWidgetsMenu = m_viewMenu->addMenu(tr("&Dock Widgets"));

        m_togglePreviewAction = new QAction(tr("Toggle Live Preview"), this);
        m_togglePreviewAction->setShortcut(QKeySequence(Qt::Key_Space));
        m_togglePreviewAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(m_togglePreviewAction, &QAction::triggered, this, [this]()
        {
            QWidget* focus = focusWidget();
            if (focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus)))
            {
                return;
            }
            if (m_previewRunning)
            {
                m_scopeonecore->stopPreview(m_currentControlTarget);
            }
            else
            {
                m_scopeonecore->startPreview(m_currentControlTarget);
            }
        });
        addAction(m_togglePreviewAction);

        m_snapAction = new QAction(tr("Snap"), this);
        m_snapAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)});
        m_snapAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(m_snapAction, &QAction::triggered, this, [this]()
        {
            m_recordingWidget->snapToGallery(m_currentControlTarget);
        });
        addAction(m_snapAction);

        m_autoContrastAction = new QAction(tr("Auto Contrast"), this);
        m_autoContrastAction->setShortcut(QKeySequence(Qt::Key_A));
        m_autoContrastAction->setShortcutContext(Qt::ApplicationShortcut);
        connect(m_autoContrastAction, &QAction::triggered, this, [this]()
        {
            QWidget* focus = focusWidget();
            if (focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus)))
            {
                return;
            }
            const QString activeLayer = m_imageWorkspace->activeLayerKey();
            if (!activeLayer.isEmpty())
            {
                m_imageWorkspace->autoLayerLevels(activeLayer);
            }
        });
        addAction(m_autoContrastAction);

        m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
        m_toolRegistry->populateMenu(m_toolsMenu, this);
        m_toolsMenu->addSeparator();
        auto* pluginManagerAction = m_toolsMenu->addAction(tr("Plugin &Manager..."));
        connect(pluginManagerAction, &QAction::triggered, this, [this]()
        {
            PluginManagerDialog(this).exec();
        });
        m_settingsAction = m_toolsMenu->addAction(tr("&Settings..."));

        m_helpMenu = menuBar()->addMenu(tr("&Help"));
        auto* reportProblemAction = m_helpMenu->addAction(tr("Report a &Problem..."));
        connect(reportProblemAction, &QAction::triggered, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "https://github.com/Experimental-Microscopy-Lab/ScopeOne/issues")));
        });
        m_helpMenu->addSeparator();
        m_aboutQtAction = m_helpMenu->addAction(tr("About &Qt"));
        m_aboutAction = m_helpMenu->addAction(tr("&About ScopeOne"));
    }

    // Create the camera control dock
    void MainWindow::setupDeviceControl()
    {
        m_deviceControlWidget = new DeviceControlWidget(m_scopeonecore, this);
        m_deviceControlWidget->setImageWorkspace(m_imageWorkspace);
        m_deviceControlWidget->setPreviewWidget(m_previewWidget);
        connect(m_deviceControlWidget, &DeviceControlWidget::stageMoveFailed,
                this, [this](const QString& message)
                {
                    showStatusMessage(message, 5000);
                    qWarning().noquote() << message;
                });
    }

    // Create the image inspection dock
    void MainWindow::setupInspect()
    {
        m_inspectWidget = new InspectWidget(m_scopeonecore, m_imageWorkspace, this);
    }

    // Create the processing module dock
    void MainWindow::setupImageProcessing()
    {
        m_imageProcessingWidget = new ImageProcessingWidget(m_scopeonecore, m_imageWorkspace, this);

        m_controlDockWidget = new QDockWidget(tr("Control"), this);
        m_controlDockWidget->setWidget(m_deviceControlWidget->hardwareControlsWidget());
        m_controlDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
        addDockWidget(Qt::RightDockWidgetArea, m_controlDockWidget);

        m_viewDockWidget = new QDockWidget(tr("View"), this);
        m_viewDockWidget->setWidget(m_deviceControlWidget->imageControlsWidget());
        m_viewDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
        tabifyDockWidget(m_controlDockWidget, m_viewDockWidget);

        m_analyzeDockWidget = new QDockWidget(tr("Analyze"), this);
        m_analyzeDockWidget->setWidget(m_inspectWidget);
        m_analyzeDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
        tabifyDockWidget(m_controlDockWidget, m_analyzeDockWidget);

        m_processDockWidget = new QDockWidget(tr("Process"), this);
        m_processDockWidget->setWidget(m_imageProcessingWidget);
        m_processDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
        tabifyDockWidget(m_controlDockWidget, m_processDockWidget);

        m_consoleDockWidget = new QDockWidget(tr("Console"), this);
        m_consoleWidget = new ConsoleWidget(m_consoleDockWidget);
        m_consoleDockWidget->setWidget(m_consoleWidget);
        m_consoleDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
        tabifyDockWidget(m_controlDockWidget, m_consoleDockWidget);
        m_controlDockWidget->raise();
    }

    // Install the Qt message sink for the embedded console
    void MainWindow::setupConsole()
    {
        ConsoleWidget::installAsQtMessageSink(m_consoleWidget);
    }

    // Create the device property and config preset dock
    void MainWindow::setupPropertyBrowser()
    {
        m_propertyDockWidget = new QDockWidget(tr("Properties"), this);
        m_propertyDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea);

        m_propertyBrowser = new DevicePropertyWidget(m_scopeonecore, this);
        m_configPresetWidget = new ConfigPresetWidget(m_scopeonecore, this);

        m_propertyDockWidget->setWidget(m_propertyBrowser);
        addDockWidget(Qt::LeftDockWidgetArea, m_propertyDockWidget);

        m_configPresetDockWidget = new QDockWidget(tr("Configs"), this);
        m_configPresetDockWidget->setWidget(m_configPresetWidget);
        m_configPresetDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea);
    }

    // Create the recording control dock
    void MainWindow::setupRecording()
    {
        m_recordingDockWidget = new QDockWidget(tr("Recording"), this);
        m_recordingDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea);

        m_recordingWidget = new RecordingWidget(m_scopeonecore, this);
        m_recordingDockWidget->setWidget(m_recordingWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_recordingDockWidget);
        splitDockWidget(m_propertyDockWidget, m_recordingDockWidget, Qt::Vertical);
    }

    // Create the image gallery dock
    void MainWindow::setupImageGallery()
    {
        m_imageGalleryDockWidget = new QDockWidget(tr("Image Gallery"), this);
        m_imageGalleryDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea);
        m_imageGalleryDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_imageGalleryWidget = new ImageGalleryWidget(m_scopeonecore, this);
        m_imageGalleryDockWidget->setWidget(m_imageGalleryWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_configPresetDockWidget);
        tabifyDockWidget(m_propertyDockWidget, m_configPresetDockWidget);
        addDockWidget(Qt::LeftDockWidgetArea, m_imageGalleryDockWidget);
        tabifyDockWidget(m_propertyDockWidget, m_imageGalleryDockWidget);
        m_propertyDockWidget->raise();
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

        m_imageWorkspace->activateLiveViewer();

        m_scopeonecore->clearStaticFrames();

        if (m_currentControlTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_imageWorkspace->setVisibleLayers(rawLayerKeys(cameraIds), cameraIds.size() > 1);
            return;
        }

        if (cameraIds.contains(m_currentControlTarget))
        {
            m_imageWorkspace->setVisibleLayers(
                {scopeone::core::ScopeOneCore::rawLayerKey(m_currentControlTarget)});
            return;
        }

        m_imageWorkspace->setVisibleLayers(rawLayerKeys(cameraIds), cameraIds.size() > 1);
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

        addDock(m_propertyDockWidget, QStringLiteral("Properties"));
        addDock(m_configPresetDockWidget, QStringLiteral("Configs"));
        addDock(m_recordingDockWidget, QStringLiteral("Recording"));
        addDock(m_imageGalleryDockWidget, QStringLiteral("Image Gallery"));
        addDock(m_controlDockWidget, QStringLiteral("Control"));
        addDock(m_viewDockWidget, QStringLiteral("View"));
        addDock(m_analyzeDockWidget, QStringLiteral("Analyze"));
        addDock(m_processDockWidget, QStringLiteral("Process"));
        addDock(m_consoleDockWidget, QStringLiteral("Console"));
    }

    // Push loaded camera ids into every dependent panel
    void MainWindow::applyLoadedCameraState(const QStringList& cameraIds)
    {
        m_previewWidget->setAvailableCameraIds(cameraIds);
        m_deviceControlWidget->setControlTargets(cameraIds);
        m_deviceControlWidget->onCameraInitialized(true);
        m_inspectWidget->setAvailableCameras(cameraIds);
        m_inspectWidget->onCameraInitialized(true);

        if (cameraIds.size() > 1)
        {
            m_imageWorkspace->setVisibleLayers(rawLayerKeys(cameraIds), true);
        }
        else if (!cameraIds.isEmpty())
        {
            m_imageWorkspace->setVisibleLayers(
                {scopeone::core::ScopeOneCore::rawLayerKey(cameraIds.first())});
        }

        m_recordingWidget->setAvailableCameras(cameraIds);
        m_toolRegistry->updateActions();
    }

    // Synchronizes camera lists and UI state based on available devices
    void MainWindow::syncCameraState()
    {
        const QStringList cameraIds = m_scopeonecore->cameraIds();
        if (cameraIds.isEmpty())
        {
            applyNoCameraState();
        }
        else
        {
            applyLoadedCameraState(cameraIds);
        }
    }

    // Clear preview and panel state when no camera is available
    void MainWindow::applyNoCameraState()
    {
        m_previewWidget->setAvailableCameraIds({});
        setStatusLabelText(m_statusPreviewLabel,
                           tr("Preview: Idle"),
                           tr("Preview is idle"));
        clearCursorStatus();
        m_previewWidget->clearCrossSection();
        m_deviceControlWidget->setControlTargets({});
        m_deviceControlWidget->onCameraInitialized(false);
        m_inspectWidget->setAvailableCameras({});
        m_inspectWidget->onCameraInitialized(false);
        m_inspectWidget->clearCrossSectionProfile();

        m_recordingWidget->setAvailableCameras({});
        m_toolRegistry->updateActions();
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
        applyColorScheme(settings.value(
            QStringLiteral("Appearance/ColorScheme"),
            QStringLiteral("system")).toString());

        const qint64 recordedMaxBytes = settings
                                        .value(
                                            QStringLiteral("Recording/MaxPendingWriteBytes"), kDefaultRecordedMaxBytes)
                                        .toLongLong();
        m_scopeonecore->setRecordingMaxPendingWriteBytes(recordedMaxBytes);

        const QString adapterDirectoryKey = QStringLiteral("Hardware/MicroManagerDirectory");
        const QString adapterDirectory = settings.contains(adapterDirectoryKey)
                                             ? settings.value(adapterDirectoryKey).toString().trimmed()
                                             : detectedMicroManagerDirectory();
        if (!m_scopeonecore->setAdditionalDeviceAdapterSearchPaths(
                adapterDirectory.isEmpty() ? QStringList{} : QStringList{adapterDirectory}))
        {
            qWarning().noquote() << QStringLiteral("Ignoring invalid Micro-Manager directory: %1")
                                    .arg(adapterDirectory);
        }
    }

    // Record the application startup state in one place
    void MainWindow::logStartupSummary()
    {
        m_consoleWidget->addMessage(
            QStringLiteral("ScopeOne %1 ready, commit %2")
                .arg(QCoreApplication::applicationVersion(),
                     QStringLiteral(SCOPEONE_GIT_COMMIT)));
        m_consoleWidget->addMessage(
            QStringLiteral("Qt runtime: %1").arg(QString::fromLatin1(qVersion())));
        m_consoleWidget->addMessage(
            QStringLiteral("Application directory: %1").arg(QCoreApplication::applicationDirPath()));
        const QStringList adapterPaths = m_scopeonecore->additionalDeviceAdapterSearchPaths();
        if (!adapterPaths.isEmpty())
        {
            m_consoleWidget->addMessage(
                QStringLiteral("External device adapters: %1").arg(adapterPaths.join(QStringLiteral("; "))));
        }
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
        const QString statusText = text.trimmed().isEmpty() ? tr("x=- y=- value=-") : text;
        setStatusLabelText(m_statusCursorLabel, statusText, statusText);
    }

    void MainWindow::clearCursorStatus()
    {
        const QString statusText = tr("x=- y=- value=-");
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

        PreviewWidget* preview = m_imageWorkspace->activePreviewWidget();
        if (!preview)
        {
            clearCursorStatus();
            return;
        }

        const QVector<PreviewWidget::PreviewInteractionTarget> targets =
            preview->interactionTargetsAt(m_lastPreviewMousePos);
        if (targets.isEmpty())
        {
            clearCursorStatus();
            return;
        }

        const PreviewWidget::PreviewInteractionTarget& activeTarget = targets.constLast();
        const auto valueText = [this](const PreviewWidget::PreviewInteractionTarget& target)
        {
            int value = 0;
            return m_imageWorkspace->pixelValue(target.layerKey, target.imagePos, value)
                       ? QString::number(value)
                       : QStringLiteral("-");
        };

        QString msg;
        if (preview->layerLayoutMode() == PreviewWidget::LayerLayoutMode::SideBySide)
        {
            msg = QStringLiteral("[%1] X: %2  Y: %3 | Val: %4")
                      .arg(preview->layerName(activeTarget.layerKey))
                      .arg(activeTarget.imagePos.x())
                      .arg(activeTarget.imagePos.y())
                      .arg(valueText(activeTarget));
        }
        else
        {
            QStringList values;
            values.reserve(targets.size());
            for (const auto& target : targets)
            {
                values.append(QStringLiteral("[%1]: %2")
                                  .arg(preview->layerName(target.layerKey))
                                  .arg(valueText(target)));
            }
            msg = QStringLiteral("X: %1  Y: %2 | %3")
                      .arg(activeTarget.imagePos.x())
                      .arg(activeTarget.imagePos.y())
                      .arg(values.join(QStringLiteral(" ")));
        }
        setCursorStatus(msg);
    }

    // Coalesce live cursor sampling independently of camera frame rate
    void MainWindow::schedulePreviewCursorStatusRefresh()
    {
        if (m_lastPreviewMousePos.x() >= 0
            && m_lastPreviewMousePos.y() >= 0
            && !m_cursorRefreshTimer->isActive())
        {
            m_cursorRefreshTimer->start();
        }
    }

    // Display a line measurement using the layer to sensor transform
    void MainWindow::showMeasurementLine(const QString& layerKey,
                                         const QPoint& start,
                                         const QPoint& end)
    {
        double actualLengthUm = 0.0;
        const double pixelSizeUm = m_imageWorkspace->pixelSizeUm(layerKey);
        scopeone::core::DocumentLayer layer;
        if (pixelSizeUm > 0.0
            && m_imageWorkspace->activeSceneModel()->findLayer(layerKey, layer))
        {
            const QPointF sensorStart = layer.pixelToSensor.map(QPointF(start));
            const QPointF sensorEnd = layer.pixelToSensor.map(QPointF(end));
            actualLengthUm = std::hypot(sensorEnd.x() - sensorStart.x(),
                                        sensorEnd.y() - sensorStart.y())
                * pixelSizeUm;
        }
        m_inspectWidget->setMeasurementLine(layerKey, start, end, actualLengthUm);
    }

    // Edit persistent application settings
    void MainWindow::openSettingsDialog()
    {
        constexpr qint64 kDefaultRecordedMaxBytes = 16ll * 1024 * 1024 * 1024;
        const qint64 currentValue = m_scopeonecore->recordingMaxPendingWriteBytes();
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QString colorScheme = settings.value(
            QStringLiteral("Appearance/ColorScheme"),
            QStringLiteral("system")).toString();

        const QStringList adapterPaths = m_scopeonecore->additionalDeviceAdapterSearchPaths();
        SettingsDialog dialog(currentValue > 0 ? currentValue : kDefaultRecordedMaxBytes,
                              adapterPaths.value(0),
                              colorScheme,
                              this);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        const qint64 recordedMaxBytes = dialog.maxPendingWriteBytes();
        const QString microManagerDirectory = dialog.microManagerDirectory();
        if (!m_scopeonecore->setAdditionalDeviceAdapterSearchPaths(
                microManagerDirectory.isEmpty() ? QStringList{} : QStringList{microManagerDirectory}))
        {
            QMessageBox::warning(this,
                                 tr("Settings"),
                                 tr("The device adapter directory could not be updated."));
            return;
        }
        const QString selectedColorScheme = dialog.colorScheme();
        settings.setValue(QStringLiteral("Recording/MaxPendingWriteBytes"), recordedMaxBytes);
        settings.setValue(QStringLiteral("Hardware/MicroManagerDirectory"), microManagerDirectory);
        settings.setValue(QStringLiteral("Appearance/ColorScheme"), selectedColorScheme);
        m_scopeonecore->setRecordingMaxPendingWriteBytes(recordedMaxBytes);
        applyColorScheme(selectedColorScheme);
        showStatusMessage(
            tr("Settings updated"),
            5000);
    }

    // Display image coordinates and pixel value under the cursor
    void MainWindow::handlePreviewMousePosition(const QPoint& pos)
    {
        m_lastPreviewMousePos = pos;
        m_cursorRefreshTimer->stop();
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
        m_loadConfigurationAction->setEnabled(true);
        m_unloadConfigurationAction->setEnabled(true);
        m_recentConfigurationsMenu->setEnabled(
            !m_recentConfigurationsMenu->isEmpty()
            && !m_scopeonecore->configurationOperationRunning());

        if (!success)
        {
            QMessageBox::critical(this, tr("Load Failed"),
                                  tr("Failed to load configuration: %1").arg(errorMessage));
            showStatusMessage(tr("Configuration load failed"), 5000);
            qCritical().noquote() << QString("Configuration failed: %1").arg(errorMessage);
            return;
        }

        const QString canonicalPath = QFileInfo(configPath).canonicalFilePath();
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        QStringList recentPaths = settings.value(
            QStringLiteral("RecentConfigurations")).toStringList();
        recentPaths.removeAll(canonicalPath);
        recentPaths.prepend(canonicalPath);
        recentPaths = recentPaths.mid(0, kMaxRecentConfigurations);
        settings.setValue(QStringLiteral("RecentConfigurations"), recentPaths);
        refreshRecentConfigurationsMenu();

        if (successCount > 0)
        {
            qInfo().noquote() << QString("%1 device(s) initialized successfully").arg(successCount);
        }
        if (failCount > 0)
        {
            const QString failedDevices = m_scopeonecore->configurationFailedDevices().join(
                QStringLiteral(", "));
            const QString warning = failedDevices.isEmpty()
                                         ? tr("%1 device(s) failed to initialize").arg(failCount)
                                         : tr("%1 device(s) failed to initialize: %2")
                                               .arg(failCount)
                                               .arg(failedDevices);
            showStatusMessage(warning, 5000);
            qWarning().noquote() << warning;
        }
        if (skippedCameraCount > 0)
        {
            qInfo().noquote() << QString("%1 camera device(s) skipped for UI initialization")
                .arg(skippedCameraCount);
        }

        if (!cameraIds.isEmpty())
        {
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

        qInfo().noquote() << QString("Configuration loaded (%1): %2")
                                 .arg(m_scopeonecore->configurationState(), configPath);
    }

    // Apply UI state after a configuration unload attempt
    void MainWindow::handleConfigurationUnloadFinished(bool success,
                                                       const QString& errorMessage)
    {
        m_loadConfigurationAction->setEnabled(true);
        m_unloadConfigurationAction->setEnabled(true);
        if (!success)
        {
            QMessageBox::critical(this, tr("Unload Failed"),
                                  tr("Failed to unload configuration: %1").arg(errorMessage));
            showStatusMessage(tr("Configuration unload failed"), 5000);
            qCritical().noquote() << QString("Configuration unload failed: %1").arg(errorMessage);
            return;
        }

        qInfo().noquote() << "Configuration unload completed successfully";
    }

    // Completes the deferred close after every gallery save finishes
    void MainWindow::handleCloseSaveFinished(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
    {
        if (!m_closeSaveInProgress || !m_closePendingSessions.removeOne(session))
        {
            return;
        }

        const int completed = m_closeSaveTotal - m_closePendingSessions.size();
        if (m_closeSaveProgress)
        {
            m_closeSaveProgress->setValue(completed);
        }
        if (!session || !session->isSaved())
        {
            m_closeSaveInProgress = false;
            m_closeAfterSave = false;
            m_closePendingSessions.clear();
            if (m_closeSaveProgress)
            {
                m_closeSaveProgress->close();
                m_closeSaveProgress->deleteLater();
                m_closeSaveProgress = nullptr;
            }
            QMessageBox::critical(
                this,
                tr("Save Failed"),
                session && !session->saveMessage().isEmpty()
                    ? session->saveMessage()
                    : tr("Failed to save gallery images"));
            return;
        }
        if (!m_closePendingSessions.isEmpty())
        {
            return;
        }

        m_closeSaveInProgress = false;
        const bool closeAfterSave = m_closeAfterSave;
        m_closeAfterSave = false;
        if (m_closeSaveProgress)
        {
            m_closeSaveProgress->setValue(m_closeSaveTotal);
            m_closeSaveProgress->close();
            m_closeSaveProgress->deleteLater();
            m_closeSaveProgress = nullptr;
        }
        if (closeAfterSave)
        {
            QTimer::singleShot(0, this, &QWidget::close);
        }
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
        loadConfigurationPath(fileName);
    }

    // Load a configuration from a known path
    void MainWindow::loadConfigurationPath(const QString& configPath)
    {
        closeLoadConfigProgress();

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

        showStatusMessage(tr("Loading configuration..."));
        qInfo().noquote() << QString("Loading configuration: %1").arg(configPath);
        m_loadConfigurationAction->setEnabled(false);
        m_unloadConfigurationAction->setEnabled(false);
        m_recentConfigurationsMenu->setEnabled(false);
        if (!m_scopeonecore->loadConfiguration(configPath))
        {
            handleConfigurationLoadFinished(false,
                                            configPath,
                                            {},
                                            false,
                                            0,
                                            0,
                                            0,
                                            m_scopeonecore->configurationError());
        }
    }

    // Rebuild the recent configuration menu from persistent paths
    void MainWindow::refreshRecentConfigurationsMenu()
    {
        m_recentConfigurationsMenu->clear();

        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QStringList storedPaths = settings.value(
            QStringLiteral("RecentConfigurations")).toStringList();
        QStringList validPaths;
        for (const QString& path : storedPaths)
        {
            if (!QFileInfo(path).isFile())
            {
                continue;
            }

            validPaths.append(path);
            auto* action = m_recentConfigurationsMenu->addAction(
                QDir::toNativeSeparators(path));
            connect(action, &QAction::triggered, this, [this, path]() {
                loadConfigurationPath(path);
            });
        }

        if (validPaths != storedPaths)
        {
            settings.setValue(QStringLiteral("RecentConfigurations"), validPaths);
        }
        m_recentConfigurationsMenu->setEnabled(
            !validPaths.isEmpty() && !m_scopeonecore->configurationOperationRunning());
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
            m_loadConfigurationAction->setEnabled(false);
            m_unloadConfigurationAction->setEnabled(false);
            showStatusMessage(tr("Unloading configuration..."));
            if (!m_scopeonecore->unloadConfiguration())
            {
                handleConfigurationUnloadFinished(
                    false, m_scopeonecore->configurationError());
            }
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
