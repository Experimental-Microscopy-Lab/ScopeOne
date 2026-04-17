#include "MainWindow.h"

#include "scopeone/ScopeOneCore.h"
#include "AboutDialog.h"
#include "InspectWidget.h"
#include "ConsoleWidget.h"
#include "DeviceControlWidget.h"
#include "DevicePropertyWidget.h"
#include "PreviewWidget.h"
#include "ImageProcessingWidget.h"
#include "RecordingWidget.h"
#include "SettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
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

namespace scopeone::ui
{
    namespace
    {
        QString rawStreamKey(const QString& cameraId)
        {
            return QStringLiteral("raw:%1").arg(cameraId);
        }

        QStringList rawStreamKeys(const QStringList& cameraIds)
        {
            QStringList streamKeys;
            streamKeys.reserve(cameraIds.size());
            for (const QString& cameraId : cameraIds)
            {
                streamKeys.append(rawStreamKey(cameraId));
            }
            return streamKeys;
        }
    }

    MainWindow::MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QMainWindow(parent)
          , m_scopeonecore(core)
    {
        if (!m_scopeonecore)
        {
            qFatal("MainWindow requires ScopeOneCore");
        }

        setupUI();
        setupSignalWiring();
        applyStoredApplicationSettings();

        setWindowTitle("ScopeOne");
        setMinimumSize(1366, 768);
        resize(1600, 900);
    }

    void MainWindow::setupSignalWiring()
    {
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewStateChanged,
                this, [this](bool running)
                {
                    if (!m_deviceControlWidget)
                    {
                        return;
                    }
                    m_deviceControlWidget->setPreviewRunning(running);
                    m_deviceControlWidget->setControlTargetEnabled(!running);
                });

        if (m_previewWidget)
        {
            connect(m_previewWidget, &PreviewWidget::mousePositionChanged,
                    this, &MainWindow::handlePreviewMousePosition);
            connect(m_previewWidget, &PreviewWidget::roiDrawn,
                    this, &MainWindow::handleRoiDrawn);
            connect(m_previewWidget, &PreviewWidget::lineDrawn,
                    this, [this](const QString& cameraId,
                                 int startX,
                                 int startY,
                                 int endX,
                                 int endY,
                                 bool processed)
                    {
                        m_scopeonecore->setLineProfile(cameraId,
                                                       QPoint(startX, startY),
                                                       QPoint(endX, endY),
                                                       processed);
                    });

            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::newRawFrameReady,
                    this, [this](const scopeone::core::ImageFrame& frame)
                    {
                        const bool allow = (!m_deviceControlWidget)
                                               || m_deviceControlWidget->acceptsCameraStream(frame.cameraId);
                        if (!allow || !frame.isValid())
                        {
                            return;
                        }
                        m_previewWidget->setRawFrame(frame);
                    }, Qt::QueuedConnection);

            connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processedFrameReady,
                    m_previewWidget, &PreviewWidget::setProcessedFrame);
        }

        if (m_deviceControlWidget)
        {
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
                        if (m_previewWidget)
                        {
                            m_previewWidget->startROIDrawing(cameraId);
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
                                qInfo().noquote() << QString("ROI cleared for %1").arg(id);
                            }
                            else
                            {
                                qWarning().noquote() << QString("Failed to clear ROI for %1").arg(id);
                            }
                        }
                    });

            connect(m_deviceControlWidget, &DeviceControlWidget::controlTargetChanged,
                    this, &MainWindow::updateControlTarget);
            connect(m_deviceControlWidget, &DeviceControlWidget::controlTargetChanged,
                    this, [this](const QString& target)
                    {
                        if (m_inspectWidget)
                        {
                            m_inspectWidget->setCurrentTarget(target);
                        }
                    });

            connect(m_deviceControlWidget, &DeviceControlWidget::exposureValueChanged,
                    this, [this](double ms)
                    {
                        m_scopeonecore->setExposure(m_currentControlTarget, ms);
                    });
        }

        if (m_inspectWidget)
        {
            connect(m_inspectWidget, &InspectWidget::requestDrawCrossSection,
                    this, [this](const QString& cameraId)
                    {
                        if (m_previewWidget)
                        {
                            m_previewWidget->startLineDrawing(cameraId);
                        }
                        statusBar()->showMessage(QStringLiteral("Drag a line on the preview"));
                    });

            connect(m_inspectWidget, &InspectWidget::requestClearCrossSection,
                    this, [this]()
                    {
                        if (m_previewWidget)
                        {
                            m_previewWidget->clearLine();
                        }
                        m_scopeonecore->clearLineProfile();
                    });

            if (m_previewWidget)
            {
                connect(m_inspectWidget, &InspectWidget::displayRangeChanged,
                        m_previewWidget, &PreviewWidget::setStreamDisplayLevels);
            }
        }

        if (m_imageProcessingWidget && m_previewWidget)
        {
            connect(m_imageProcessingWidget, &ImageProcessingWidget::processingStarted,
                    this, [this]()
                    {
                        QStringList selectedStreams = m_previewWidget->selectedStreams();
                        const QStringList availableCameraIds = m_previewWidget->availableCameraIds();

                        for (const QString& streamKey : std::as_const(selectedStreams))
                        {
                            if (!streamKey.startsWith(QStringLiteral("raw:")))
                            {
                                continue;
                            }
                            const QString cameraId = streamKey.mid(4);
                            if (cameraId.isEmpty() || !availableCameraIds.contains(cameraId))
                            {
                                continue;
                            }
                            const QString processedStream = QStringLiteral("proc:%1").arg(cameraId);
                            if (!selectedStreams.contains(processedStream))
                            {
                                selectedStreams.append(processedStream);
                            }
                        }

                        if (selectedStreams.isEmpty())
                        {
                            for (const QString& cameraId : availableCameraIds)
                            {
                                selectedStreams.append(QStringLiteral("proc:%1").arg(cameraId));
                            }
                        }

                        m_previewWidget->setSelectedStreams(selectedStreams);
                        m_previewWidget->setStreamLayoutMode(PreviewWidget::StreamLayoutMode::SideBySide);
                    });
        }

        if (m_exitAction)
        {
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
        }

        if (m_propertyBrowser)
        {
            connect(m_propertyBrowser, &DevicePropertyWidget::propertyChanged,
                    this, [](const QString& device, const QString& property, const QString& value)
                    {
                        qInfo().noquote() << QString("Property changed: %1.%2 = %3")
                            .arg(device, property, value);
                    });

            connect(m_propertyBrowser, &DevicePropertyWidget::errorOccurred,
                    this, [](const QString& message)
                    {
                        qCritical().noquote() << message;
                    });
        }
    }

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
        updateDockWidgetMenu();
    }

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
        m_settingsAction = m_toolsMenu->addAction(tr("&Settings..."));

        m_helpMenu = menuBar()->addMenu(tr("&Help"));
        m_aboutQtAction = m_helpMenu->addAction(tr("About &Qt"));
        m_aboutAction = m_helpMenu->addAction(tr("&About ScopeOne"));
    }

    void MainWindow::setupDeviceControl()
    {
        m_deviceControlDockWidget = new QDockWidget(tr("Control"), this);
        m_deviceControlWidget = new DeviceControlWidget(m_scopeonecore, this);
        m_deviceControlWidget->setPreviewWidget(m_previewWidget);
        m_deviceControlDockWidget->setWidget(m_deviceControlWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_deviceControlDockWidget);
    }

    void MainWindow::setupInspect()
    {
        m_inspectDockWidget = new QDockWidget(tr("Inspect"), this);
        m_inspectDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_inspectWidget = new InspectWidget(m_scopeonecore, this);
        m_inspectDockWidget->setWidget(m_inspectWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_inspectDockWidget);
        if (m_deviceControlDockWidget)
        {
            tabifyDockWidget(m_deviceControlDockWidget, m_inspectDockWidget);
        }
    }

    void MainWindow::setupImageProcessing()
    {
        m_imageProcessingDockWidget = new QDockWidget(tr("Image Processing"), this);
        m_imageProcessingDockWidget->setFeatures(QDockWidget::DockWidgetMovable |
            QDockWidget::DockWidgetFloatable |
            QDockWidget::DockWidgetClosable);

        m_imageProcessingWidget = new ImageProcessingWidget(m_scopeonecore, this);
        m_imageProcessingDockWidget->setWidget(m_imageProcessingWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_imageProcessingDockWidget);
        if (m_inspectDockWidget)
        {
            tabifyDockWidget(m_inspectDockWidget, m_imageProcessingDockWidget);
        }
        else
        {
            tabifyDockWidget(m_deviceControlDockWidget, m_imageProcessingDockWidget);
        }
        m_deviceControlDockWidget->raise();
    }

    void MainWindow::setupConsole()
    {
        m_consoleDockWidget = new QDockWidget(tr("Console"), this);
        m_consoleWidget = new ConsoleWidget(this);
        m_consoleDockWidget->setWidget(m_consoleWidget);

        ConsoleWidget::installAsQtMessageSink(m_consoleWidget);

        addDockWidget(Qt::RightDockWidgetArea, m_consoleDockWidget);
        splitDockWidget(m_deviceControlDockWidget, m_consoleDockWidget, Qt::Vertical);
    }

    void MainWindow::setupPropertyBrowser()
    {
        m_propertyDockWidget = new QDockWidget(tr("Device Properties"), this);
        m_propertyDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

        m_propertyBrowser = new DevicePropertyWidget(m_scopeonecore, this);
        m_propertyDockWidget->setWidget(m_propertyBrowser);

        addDockWidget(Qt::LeftDockWidgetArea, m_propertyDockWidget);
    }

    void MainWindow::setupRecording()
    {
        m_recordingDockWidget = new QDockWidget(tr("Recording"), this);
        m_recordingDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

        m_recordingWidget = new RecordingWidget(m_scopeonecore, this);
        m_recordingDockWidget->setWidget(m_recordingWidget);

        addDockWidget(Qt::LeftDockWidgetArea, m_recordingDockWidget);
        if (m_propertyDockWidget)
        {
            splitDockWidget(m_propertyDockWidget, m_recordingDockWidget, Qt::Vertical);
        }
    }

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

    void MainWindow::updateControlTarget(const QString& target)
    {
        // Keep preview on the chosen target
        const QString normalizedTarget = target.trimmed();
        if (normalizedTarget.isEmpty())
        {
            return;
        }
        m_currentControlTarget = normalizedTarget;

        if (!m_previewWidget)
        {
            return;
        }

        const QStringList cameraIds = m_scopeonecore->cameraIds();
        if (normalizedTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_previewWidget->setSelectedStreams(rawStreamKeys(cameraIds));
            return;
        }

        m_previewWidget->setSelectedStreams({rawStreamKey(normalizedTarget)});

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

    void MainWindow::updateDockWidgetMenu()
    {
        if (!m_dockWidgetsMenu)
        {
            return;
        }

        m_dockWidgetsMenu->clear();
        const auto addDock = [this](QDockWidget* dockWidget, const QString& label)
        {
            if (!dockWidget || !m_dockWidgetsMenu)
            {
                return;
            }
            QAction* action = dockWidget->toggleViewAction();
            action->setText(label);
            m_dockWidgetsMenu->addAction(action);
        };

        addDock(m_propertyDockWidget, QStringLiteral("Device Properties"));
        addDock(m_recordingDockWidget, QStringLiteral("Recording"));
        addDock(m_consoleDockWidget, QStringLiteral("Console"));
        addDock(m_deviceControlDockWidget, QStringLiteral("Control"));
        addDock(m_inspectDockWidget, QStringLiteral("Inspect"));
        addDock(m_imageProcessingDockWidget, QStringLiteral("Image Processing"));
    }

    void MainWindow::applyLoadedCameraState(const QStringList& cameraIds)
    {
        if (m_deviceControlWidget)
        {
            m_deviceControlWidget->onCameraInitialized(true);
            m_deviceControlWidget->setControlTargets(cameraIds);
        }
        if (m_inspectWidget)
        {
            m_inspectWidget->onCameraInitialized(true);
            m_inspectWidget->setAvailableCameras(cameraIds);
        }

        if (m_previewWidget)
        {
            m_previewWidget->setAvailableCameraIds(cameraIds);
        }

        if (m_previewWidget && !cameraIds.isEmpty())
        {
            m_previewWidget->setSelectedStreams({rawStreamKey(cameraIds.first())});
        }

        if (m_recordingWidget)
        {
            m_recordingWidget->setAvailableCameras(cameraIds);
        }
    }

    void MainWindow::applyUnloadedCameraState(const QStringList& cameraIds)
    {
        if (m_previewWidget)
        {
            for (const QString& id : cameraIds)
            {
                m_previewWidget->clearCameraFrames(id);
            }
            m_previewWidget->setAvailableCameraIds({});
            m_previewWidget->clearLine();
        }
        if (m_deviceControlWidget)
        {
            m_deviceControlWidget->setControlTargets({});
            m_deviceControlWidget->onCameraInitialized(false);
        }
        if (m_inspectWidget)
        {
            m_inspectWidget->setAvailableCameras({});
            m_inspectWidget->onCameraInitialized(false);
            m_inspectWidget->clearCrossSectionProfile();
        }

        if (m_recordingWidget)
        {
            m_recordingWidget->setAvailableCameras({});
        }
    }

    void MainWindow::refreshDevicePanels(bool fromCache)
    {
        if (m_propertyBrowser)
        {
            m_propertyBrowser->refresh(fromCache);
        }
        if (m_deviceControlWidget)
        {
            m_deviceControlWidget->refreshStageDevices();
        }
    }

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

    void MainWindow::handlePreviewMousePosition(const QPoint& pos)
    {
        if (!m_previewWidget)
        {
            return;
        }
        if (pos.x() < 0 || pos.y() < 0)
        {
            statusBar()->clearMessage();
            return;
        }

        QString cameraId;
        QPoint imagePos;
        bool processed = false;
        bool ok = false;
        if (m_currentControlTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) != 0)
        {
            ok = m_previewWidget->widgetToImageCoordsForCamera(m_currentControlTarget, pos, imagePos, processed);
            if (ok)
            {
                cameraId = m_currentControlTarget;
            }
        }
        else
        {
            ok = m_previewWidget->widgetToImageCoords(pos, cameraId, imagePos, processed);
        }

        if (!ok)
        {
            statusBar()->showMessage(QString("Pos: (%1,%2) | Value: -").arg(pos.x()).arg(pos.y()));
            return;
        }

        int value = 0;
        const bool valueOk = m_previewWidget->getPixelValue(cameraId, imagePos, processed, value);
        const QString stream = processed ? QStringLiteral("proc") : QStringLiteral("raw");
        const QString msg = valueOk
                                ? QString("%1 [%2] Pos: (%3,%4) | Value: %5")
                                  .arg(cameraId, stream)
                                  .arg(imagePos.x()).arg(imagePos.y())
                                  .arg(value)
                                : QString("%1 [%2] Pos: (%3,%4) | Value: -")
                                  .arg(cameraId, stream)
                                  .arg(imagePos.x()).arg(imagePos.y());
        statusBar()->showMessage(msg);
    }

    void MainWindow::handleRoiDrawn(const QString& cameraId, int x, int y, int width, int height)
    {
        // Backend restarts preview for ROI
        const bool success = m_scopeonecore->setROI(cameraId, x, y, width, height);
        if (success)
        {
            qInfo().noquote() << QString("ROI set for %1: %2x%3 at (%4,%5)")
                                 .arg(cameraId).arg(width).arg(height).arg(x).arg(y);
        }
        else
        {
            qWarning().noquote() << QString("Failed to set ROI for %1").arg(cameraId);
        }
    }

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
