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
#include <QStyleHints>
#include <QTabWidget>
#include <QTimer>
#include <QVector>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace scopeone::ui
{
    using ImageSceneModel = scopeone::core::ImageSceneModel;

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

        // Build a stable key for one gallery session
        QString gallerySessionKey(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return session ? session->capturePlan().experimentId : QString();
        }

        // Build the static layer id for one recorded camera in a gallery session
        QString gallerySessionLayerId(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& cameraId)
        {
            return QStringLiteral("gallery:%1:%2").arg(gallerySessionKey(session), cameraId);
        }

        int uiFrameCount(qint64 frameCount)
        {
            return static_cast<int>(
                qBound<qint64>(1, frameCount, static_cast<qint64>((std::numeric_limits<int>::max)())));
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
        setupUI();
        setupSignalWiring();
        new ScopeOneLocalApiServer(m_scopeonecore, m_previewWidget, this);
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
            m_closeSaveProgress = new QProgressDialog(
                tr("Saving gallery images..."), QString(), 0, m_closeSaveTotal, this);
            m_closeSaveProgress->setWindowTitle(tr("Saving"));
            m_closeSaveProgress->setWindowModality(Qt::ApplicationModal);
            m_closeSaveProgress->setCancelButton(nullptr);
            m_closeSaveProgress->setMinimumDuration(0);
            m_closeSaveProgress->setAutoClose(false);
            m_closeSaveProgress->setValue(0);
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
                    m_propertyBrowser->setEnabled(!configurationRunning);
                    m_configPresetWidget->setEnabled(!configurationRunning);
                    m_deviceControlWidget->setEnabled(!configurationRunning);
                    m_scaleAction->setEnabled(!configurationRunning && !m_scopeonecore->cameraIds().isEmpty());
                    m_stageMosaicAction->setEnabled(!configurationRunning);
                    m_particleDetectionAction->setEnabled(!configurationRunning);
                    if (m_stageMosaicDialog)
                    {
                        m_stageMosaicDialog->setEnabled(!configurationRunning);
                    }
                    if (m_particleDetectionDialog)
                    {
                        m_particleDetectionDialog->setEnabled(!configurationRunning);
                    }

                    const QStringList cameraIds = m_scopeonecore->cameraIds();
                    if (cameraIds.isEmpty())
                    {
                        applyNoCameraState();
                    }
                    else
                    {
                        applyLoadedCameraState(cameraIds);
                    }
                    if (!configurationRunning)
                    {
                        refreshDevicePanels(false);
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
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionFrameReady,
                this, &MainWindow::handleGalleryFrameReady);
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
                    m_previewWidget->resetLiveFrameRates();
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
                    m_galleryLayerFrameControls.remove(layerKey);
                    m_galleryFrameRequests.remove(layerKey);
                    m_deviceControlWidget->removeLayerFrameControl(layerKey);
                    m_previewWidget->removeStaticLayer(layerKey);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::staticFramesCleared,
                this, [this]()
                {
                    m_galleryFrameRequests.clear();
                    for (const QString& layerKey : m_galleryLayerFrameControls.keys())
                    {
                        m_deviceControlWidget->removeLayerFrameControl(layerKey);
                    }
                    m_galleryLayerFrameControls.clear();
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

        connect(m_deviceControlWidget, &DeviceControlWidget::controlTargetChanged,
                this, &MainWindow::updateControlTarget);
        connect(m_deviceControlWidget, &DeviceControlWidget::currentLayerChanged,
                m_inspectWidget, &InspectWidget::setCurrentLayer);
        connect(m_deviceControlWidget, &DeviceControlWidget::previewLayerFrameRequested,
                this, &MainWindow::updateGalleryLayerFrame);
        connect(m_previewWidget, &PreviewWidget::availableLayerKeysChanged,
                m_inspectWidget, &InspectWidget::setAvailableLayers);
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
                    m_previewWidget->startCrossSectionDrawingForLayer(layerKey);
                    showStatusMessage(tr("Drag a line on the preview"), 5000);
                });

        connect(m_inspectWidget, &InspectWidget::requestClearCrossSection,
                this, [this]()
                {
                    m_previewWidget->clearCrossSection();
                });

        connect(m_inspectWidget, &InspectWidget::requestDrawMeasurementLine,
                this, [this](const QString& layerKey)
                {
                    m_previewWidget->startMeasurementLineDrawingForLayer(layerKey);
                    showStatusMessage(tr("Drag a line on the preview"), 5000);
                });
        connect(m_inspectWidget, &InspectWidget::requestClearMeasurementLines,
                this, [this](const QString& layerKey)
                {
                    m_imageSceneModel->clearRole(ImageSceneModel::MarkupRole::Measurement, layerKey);
                    m_inspectWidget->clearMeasurementLine();
                });
        connect(m_previewWidget, &PreviewWidget::measurementLineDrawn,
                this, [this](const QString& layerKey, const QPoint& start, const QPoint& end)
                {
                    const QString markupId = m_imageSceneModel->createLine(
                        layerKey,
                        start,
                        end,
                        QString(),
                        ImageSceneModel::MarkupRole::Measurement);
                    m_imageSceneModel->selectOnly(markupId);
                    showMeasurementLine(layerKey, start, end);
                });
        connect(m_previewWidget, &PreviewWidget::measurementLineInspected,
                this, [this](const QString& layerKey,
                             const QPoint& start,
                             const QPoint& end)
                {
                    showMeasurementLine(layerKey, start, end);
                });
        connect(m_previewWidget, &PreviewWidget::measurementLineCleared,
                m_inspectWidget, &InspectWidget::clearMeasurementLine);
        connect(m_imageSceneModel, &ImageSceneModel::markupsChanged,
                this, [this]()
                {
                    for (const ImageSceneModel::Markup& markup : m_imageSceneModel->markups())
                    {
                        if (markup.selected
                            && markup.type == ImageSceneModel::MarkupType::Line
                            && markup.role == ImageSceneModel::MarkupRole::Measurement)
                        {
                            showMeasurementLine(markup.layerKey, markup.start, markup.end);
                            return;
                        }
                    }
                    m_inspectWidget->clearMeasurementLine();
                });

        m_inspectWidget->setAvailableLayers(m_previewWidget->availableLayerKeys());
        m_inspectWidget->setCurrentLayer(m_deviceControlWidget->currentLayerKey());

        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStarted,
                this, [this]()
                {
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Live"),
                                       tr("Processing is running"));
                    showStatusMessage(tr("Image processing started"), 3000);
                    QStringList visibleLayerKeys = m_previewWidget->visibleLayerKeys();
                    const QStringList availableCameraIds = m_previewWidget->availableCameraIds();

                    for (const QString& layerKey : std::as_const(visibleLayerKeys))
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
                        if (!visibleLayerKeys.contains(processedLayerKey))
                        {
                            visibleLayerKeys.append(processedLayerKey);
                        }
                    }

                    if (visibleLayerKeys.isEmpty())
                    {
                        for (const QString& cameraId : availableCameraIds)
                        {
                            visibleLayerKeys.append(scopeone::core::ScopeOneCore::processedLayerKey(cameraId));
                        }
                    }

                    m_imageSceneModel->setVisibleLayers(visibleLayerKeys);
                    m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::SideBySide);
                });
        connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStopped,
                this, [this]()
                {
                    setStatusLabelText(m_statusProcessingLabel,
                                       tr("Processing: Off"),
                                       tr("Processing is off"));
                    showStatusMessage(tr("Image processing stopped"), 3000);
                    QStringList visibleLayerKeys = rawOnlyLayerKeys(m_previewWidget->visibleLayerKeys());
                    if (visibleLayerKeys.isEmpty())
                    {
                        visibleLayerKeys = rawLayerKeys(m_previewWidget->availableCameraIds());
                    }
                    m_imageSceneModel->setVisibleLayers(visibleLayerKeys);
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
        connect(m_scaleAction, &QAction::triggered,
                this, &MainWindow::openScaleDialog);
        connect(m_settingsAction, &QAction::triggered,
                this, &MainWindow::openSettingsDialog);

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::stageMosaicFrameUpdated,
                this, [this](const scopeone::core::ImageFrame&)
                {
                    const QString layerKey =
                        scopeone::core::ScopeOneCore::staticLayerKey(QStringLiteral("stage_mosaic"));
                    m_imageSceneModel->setLayerColormap(layerKey, QStringLiteral("Gray"));
                    m_imageSceneModel->setLayerBlending(layerKey, QStringLiteral("Opaque"));
                    m_imageSceneModel->setVisibleLayers({layerKey});
                    m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::stageMosaicFinished,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                       const QString& message,
                       bool)
                {
                    if (!session)
                    {
                        showStatusMessage(message, 8000);
                        return;
                    }
                    const QString title = tr("Stage Mosaic %1").arg(session->cameraIds().value(0));
                    m_imageGalleryWidget->addSession(session, title);
                    m_scopeonecore->removeStaticFrame(QStringLiteral("stage_mosaic"));
                    registerGallerySessionFrameControls(session, 0);
                    for (const QString& cameraId : session->recordedCameraIds())
                    {
                        if (session->recordedFrameCount(cameraId) > 0)
                        {
                            updateGalleryLayerFrame(
                                scopeone::core::ScopeOneCore::staticLayerKey(
                                    gallerySessionLayerId(session, cameraId)),
                                0);
                        }
                    }
                    showStatusMessage(tr("Mosaic added to Gallery"), 5000);
                });

        connect(m_recordingWidget, &RecordingWidget::gallerySessionCaptured,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    updateSessionPresentation(*m_scopeonecore, *m_imageSceneModel, session);
                    m_imageGalleryWidget->addSession(session);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    m_imageGalleryWidget->addSession(session);
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
                    registerGallerySessionFrameControls(session, 0);
                    for (const QString& cameraId : session->recordedCameraIds())
                    {
                        if (session->recordedFrameCount(cameraId) > 0)
                        {
                            updateGalleryLayerFrame(
                                scopeone::core::ScopeOneCore::staticLayerKey(
                                    gallerySessionLayerId(session, cameraId)),
                                0);
                        }
                    }
                    showStatusMessage(tr("Loading gallery preview..."));
                });
        connect(m_imageGalleryWidget, &ImageGalleryWidget::livePreviewRequested,
                this, &MainWindow::showLivePreview);
        connect(m_imageGalleryWidget, &ImageGalleryWidget::sessionRemoved,
                this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    removeGallerySessionFrameControls(session);
                    removeGallerySessionPreview(*m_scopeonecore, session);
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
        m_unloadConfigurationAction = m_fileMenu->addAction(tr("&Unload Configuration"));
        m_fileMenu->addSeparator();
        m_exitAction = m_fileMenu->addAction(tr("E&xit"));

        m_viewMenu = menuBar()->addMenu(tr("&View"));
        m_fullScreenAction = m_viewMenu->addAction(tr("&Full Screen"));
        m_fullScreenAction->setCheckable(true);
        m_viewMenu->addSeparator();
        m_dockWidgetsMenu = m_viewMenu->addMenu(tr("&Dock Widgets"));

        m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
        m_scaleAction = m_toolsMenu->addAction(tr("&Scale..."));
        m_scaleAction->setEnabled(!m_scopeonecore->cameraIds().isEmpty());
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
        connect(m_deviceControlWidget, &DeviceControlWidget::stageMoveFailed,
                this, [this](const QString& message)
                {
                    showStatusMessage(message, 5000);
                    qWarning().noquote() << message;
                });
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

        m_imageGalleryWidget = new ImageGalleryWidget(m_scopeonecore, this);
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
            m_imageSceneModel->setVisibleLayers(rawLayerKeys(cameraIds));
            m_previewWidget->setLayerLayoutMode(cameraIds.size() > 1
                                                    ? PreviewWidget::LayerLayoutMode::SideBySide
                                                    : PreviewWidget::LayerLayoutMode::Overlay);
            return;
        }

        if (cameraIds.contains(m_currentControlTarget))
        {
            m_imageSceneModel->setVisibleLayers(
                {scopeone::core::ScopeOneCore::rawLayerKey(m_currentControlTarget)});
            m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
            return;
        }

        m_imageSceneModel->setVisibleLayers(rawLayerKeys(cameraIds));
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
        m_previewWidget->setAvailableCameraIds(cameraIds);
        m_deviceControlWidget->setControlTargets(cameraIds);
        m_deviceControlWidget->onCameraInitialized(true);
        m_inspectWidget->setAvailableCameras(cameraIds);
        m_inspectWidget->onCameraInitialized(true);

        if (cameraIds.size() > 1)
        {
            m_imageSceneModel->setVisibleLayers(rawLayerKeys(cameraIds));
        }
        else if (!cameraIds.isEmpty())
        {
            m_imageSceneModel->setVisibleLayers(
                {scopeone::core::ScopeOneCore::rawLayerKey(cameraIds.first())});
        }

        m_recordingWidget->setAvailableCameras(cameraIds);
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

        PreviewWidget::PreviewInteractionTarget target;
        if (!m_previewWidget->interactionTargetAt(m_lastPreviewMousePos, target))
        {
            clearCursorStatus();
            return;
        }

        int value = 0;
        const bool valueOk = m_scopeonecore->graphPixelValue(target.layerKey, target.imagePos, value);
        const QString msg = QStringLiteral("x=%1 y=%2 value=%3")
                                .arg(target.imagePos.x(), 5, 10, QLatin1Char(' '))
                                .arg(target.imagePos.y(), 5, 10, QLatin1Char(' '))
                                .arg(valueOk ? QString::number(value) : QStringLiteral("-"),
                                     6,
                                     QLatin1Char(' '));
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
        double pixelSizeUm = 0.0;
        const auto galleryControl = m_galleryLayerFrameControls.constFind(layerKey);
        if (galleryControl != m_galleryLayerFrameControls.constEnd()
            && galleryControl->session)
        {
            pixelSizeUm = galleryControl->session->cameraPixelSizeUm(galleryControl->cameraId);
        }
        else
        {
            const QString cameraId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
            pixelSizeUm = m_scopeonecore->cameraPixelSizeUm(cameraId);
        }
        scopeone::core::DocumentLayer layer;
        if (pixelSizeUm > 0.0 && m_imageSceneModel->findLayer(layerKey, layer))
        {
            const QPointF sensorStart = layer.pixelToSensor.map(QPointF(start));
            const QPointF sensorEnd = layer.pixelToSensor.map(QPointF(end));
            actualLengthUm = std::hypot(sensorEnd.x() - sensorStart.x(),
                                        sensorEnd.y() - sensorStart.y())
                * pixelSizeUm;
        }
        m_inspectWidget->setMeasurementLine(layerKey, start, end, actualLengthUm);
    }

    // Registers right panel frame sliders for stack backed gallery layers
    void MainWindow::registerGallerySessionFrameControls(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
        int frameIndex)
    {
        if (!session)
        {
            return;
        }

        for (const QString& cameraId : session->recordedCameraIds())
        {
            const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(
                gallerySessionLayerId(session, cameraId));
            const int frameCount = uiFrameCount(session->recordedFrameCount(cameraId));
            m_galleryLayerFrameControls.insert(layerKey, {session, cameraId});
            if (frameCount <= 1)
            {
                m_deviceControlWidget->removeLayerFrameControl(layerKey);
                continue;
            }

            m_deviceControlWidget->setLayerFrameControl(
                layerKey,
                frameCount,
                qBound(0, frameIndex, frameCount - 1));
        }
    }

    // Removes right panel frame sliders for one gallery session
    void MainWindow::removeGallerySessionFrameControls(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
    {
        if (!session)
        {
            return;
        }

        for (const QString& cameraId : session->recordedCameraIds())
        {
            const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(
                gallerySessionLayerId(session, cameraId));
            m_galleryLayerFrameControls.remove(layerKey);
            m_galleryFrameRequests.remove(layerKey);
            m_deviceControlWidget->removeLayerFrameControl(layerKey);
        }
    }

    // Updates gallery static layers when a layer frame slider moves
    void MainWindow::updateGalleryLayerFrame(const QString& layerKey, int frameIndex)
    {
        const auto it = m_galleryLayerFrameControls.constFind(layerKey);
        if (it == m_galleryLayerFrameControls.constEnd())
        {
            return;
        }

        const auto session = it.value().session;
        if (!session)
        {
            return;
        }

        const QString cameraId = it.value().cameraId;
        const qint64 cameraFrameCount = session->recordedFrameCount(cameraId);
        if (cameraFrameCount <= 0)
        {
            return;
        }

        const int cameraFrameIndex = static_cast<int>(
            qBound<qint64>(0, static_cast<qint64>(frameIndex), cameraFrameCount - 1));
        GalleryFrameRequestState& state = m_galleryFrameRequests[layerKey];
        state.latestFrameIndex = cameraFrameIndex;
        if (state.requestId == 0)
        {
            requestLatestGalleryFrame(layerKey);
        }
    }

    // Starts the newest pending frame read for one gallery layer
    void MainWindow::requestLatestGalleryFrame(const QString& layerKey)
    {
        const auto controlIt = m_galleryLayerFrameControls.constFind(layerKey);
        auto stateIt = m_galleryFrameRequests.find(layerKey);
        if (controlIt == m_galleryLayerFrameControls.constEnd()
            || stateIt == m_galleryFrameRequests.end()
            || stateIt->requestId != 0)
        {
            return;
        }

        stateIt->requestId = m_scopeonecore->requestRecordingSessionFrame(
            controlIt->session,
            controlIt->cameraId,
            stateIt->latestFrameIndex);
    }

    // Displays a decoded frame only if it is still the latest slider request
    void MainWindow::handleGalleryFrameReady(
        quint64 requestId,
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
        const QString& cameraId,
        int frameIndex,
        const scopeone::core::ImageFrame& frame)
    {
        const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(
            gallerySessionLayerId(session, cameraId));
        auto stateIt = m_galleryFrameRequests.find(layerKey);
        if (stateIt == m_galleryFrameRequests.end() || stateIt->requestId != requestId)
        {
            return;
        }

        stateIt->requestId = 0;
        if (stateIt->latestFrameIndex != frameIndex)
        {
            requestLatestGalleryFrame(layerKey);
            return;
        }
        if (!frame.isValid())
        {
            showStatusMessage(tr("Failed to load gallery frame"), 5000);
            return;
        }

        const QString layerId = gallerySessionLayerId(session, cameraId);
        const qint64 cameraFrameCount = session->recordedFrameCount(cameraId);
        const QString displayName = cameraFrameCount > 1
                                        ? tr("Gallery %1 Frame %2").arg(cameraId).arg(frameIndex + 1)
                                        : tr("Gallery %1").arg(cameraId);
        const scopeone::core::ImageFrame graphFrame = m_scopeonecore->publishStaticFrame(
            layerId,
            frame,
            displayName);
        if (!graphFrame.isValid())
        {
            return;
        }

        QStringList visibleLayers = m_imageSceneModel->visibleLayerIds();
        if (!visibleLayers.contains(layerKey))
        {
            visibleLayers.append(layerKey);
            m_imageSceneModel->setVisibleLayers(visibleLayers);
        }
        m_previewWidget->setLayerLayoutMode(
            visibleLayers.size() > 1
                ? PreviewWidget::LayerLayoutMode::SideBySide
                : PreviewWidget::LayerLayoutMode::Overlay);
        if (cameraFrameCount > 1)
        {
            m_deviceControlWidget->setLayerFrameControl(
                layerKey, uiFrameCount(cameraFrameCount), frameIndex);
        }
        showStatusMessage(tr("Gallery %1 frame %2").arg(cameraId).arg(frameIndex + 1), 1500);
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

    // Edit the global per camera image scale
    void MainWindow::openScaleDialog()
    {
        CameraScaleDialog dialog(m_scopeonecore, this);
        dialog.exec();
    }

    // Open the stage driven image mosaic tool
    void MainWindow::openStageMosaicTool()
    {
        if (!m_stageMosaicDialog)
        {
            auto* dialog = new StageMosaicDialog(m_scopeonecore, m_previewWidget, this);
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

        qInfo().noquote() << QString("Configuration loaded successfully: %1").arg(configPath);
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
        if (m_closeSaveProgress)
        {
            m_closeSaveProgress->setValue(m_closeSaveTotal);
            m_closeSaveProgress->close();
            m_closeSaveProgress->deleteLater();
            m_closeSaveProgress = nullptr;
        }
        QTimer::singleShot(0, this, &QWidget::close);
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
        qInfo().noquote() << QString("Loading configuration: %1").arg(fileName);
        m_loadConfigurationAction->setEnabled(false);
        m_unloadConfigurationAction->setEnabled(false);
        if (!m_scopeonecore->loadConfiguration(fileName))
        {
            handleConfigurationLoadFinished(false,
                                            fileName,
                                            {},
                                            false,
                                            0,
                                            0,
                                            0,
                                            tr("Another hardware operation is still running"));
        }
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
                    false, tr("Another hardware operation is still running"));
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
