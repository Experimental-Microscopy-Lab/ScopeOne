#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <memory>

class QAction;
class QCloseEvent;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressDialog;
class QTimer;

namespace scopeone::core
{
    class ScopeOneCore;
}

namespace scopeone::ui
{
    class DevicePropertyWidget;
    class ConfigPresetWidget;
    class ImageGalleryWidget;
    class ImageProcessingWidget;
    class InspectWidget;
    class PreviewWidget;
    class ConsoleWidget;
    class DeviceControlWidget;
    class RecordingWidget;
    class StageMosaicDialog;
    class ParticleDetectionDialog;

    class MainWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        explicit MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~MainWindow() override = default;

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        void setupUI();
        void setupSignalWiring();
        void setupStatusBar();

        void setupMenuBar();
        void setupDeviceControl();
        void setupInspect();
        void setupImageProcessing();
        void setupImageGallery();
        void setupConsole();
        void setupPropertyBrowser();
        void setupRecording();

        void closeLoadConfigProgress();
        void showLivePreview();
        void updateControlTarget(const QString& target);
        void updateDockWidgetMenu();
        void applyLoadedCameraState(const QStringList& cameraIds);
        void applyUnloadedCameraState(const QStringList& cameraIds);
        void refreshDevicePanels(bool fromCache = false);
        void applyStoredApplicationSettings();
        void logStartupSummary();
        void openSettingsDialog();
        void openStageMosaicTool();
        void openParticleDetectionTool();
        void connectPropertyPanels();
        void showStatusMessage(const QString& message, int timeoutMs = 0);
        void setCursorStatus(const QString& text);
        void clearCursorStatus();
        void refreshPreviewCursorStatus();
        void registerGallerySessionFrameControls(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            int frameIndex);
        void removeGallerySessionFrameControls(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        void updateGalleryLayerFrame(const QString& layerKey, int frameIndex);

        void handlePreviewMousePosition(const QPoint& pos);
        void handleRoiDrawn(const QString& cameraId,
                            int x,
                            int y,
                            int width,
                            int height,
                            int sourceRoiX,
                            int sourceRoiY);
        void handleConfigurationLoadFinished(bool success,
                                             const QString& configPath,
                                             const QStringList& cameraIds,
                                             bool foundCamera,
                                             int successCount,
                                             int failCount,
                                             int skippedCameraCount,
                                             const QString& errorMessage);
        void handleConfigurationUnloadFinished(bool success,
                                               const QStringList& cameraIds,
                                               const QString& errorMessage);
        void loadConfigurationFromDialog();
        void unloadConfigurationWithConfirmation();
        void setFullScreenEnabled(bool enabled);

        PreviewWidget* m_previewWidget{nullptr};
        QDockWidget* m_consoleDockWidget{nullptr};
        ConsoleWidget* m_consoleWidget{nullptr};

        QDockWidget* m_propertyDockWidget{nullptr};
        DevicePropertyWidget* m_propertyBrowser{nullptr};
        ConfigPresetWidget* m_configPresetWidget{nullptr};

        QDockWidget* m_recordingDockWidget{nullptr};
        RecordingWidget* m_recordingWidget{nullptr};
        QDockWidget* m_imageGalleryDockWidget{nullptr};
        ImageGalleryWidget* m_imageGalleryWidget{nullptr};

        struct GalleryLayerFrameControl
        {
            std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> session;
            QString cameraId;
        };
        QHash<QString, GalleryLayerFrameControl> m_galleryLayerFrameControls;

        QDockWidget* m_deviceControlDockWidget{nullptr};
        DeviceControlWidget* m_deviceControlWidget{nullptr};

        QDockWidget* m_imageProcessingDockWidget{nullptr};
        ImageProcessingWidget* m_imageProcessingWidget{nullptr};

        QDockWidget* m_inspectDockWidget{nullptr};
        InspectWidget* m_inspectWidget{nullptr};

        QMenu* m_fileMenu{nullptr};
        QMenu* m_viewMenu{nullptr};
        QMenu* m_toolsMenu{nullptr};
        QMenu* m_helpMenu{nullptr};
        QMenu* m_dockWidgetsMenu{nullptr};

        QAction* m_exitAction{nullptr};
        QAction* m_fullScreenAction{nullptr};
        QAction* m_loadConfigurationAction{nullptr};
        QAction* m_unloadConfigurationAction{nullptr};
        QAction* m_settingsAction{nullptr};
        QAction* m_stageMosaicAction{nullptr};
        QAction* m_particleDetectionAction{nullptr};
        QAction* m_aboutAction{nullptr};
        QAction* m_aboutQtAction{nullptr};

        QPointer<QProgressDialog> m_loadConfigProgress;
        QPointer<StageMosaicDialog> m_stageMosaicDialog;
        QPointer<ParticleDetectionDialog> m_particleDetectionDialog;
        QLabel* m_statusMessageLabel{nullptr};
        QLabel* m_statusCursorLabel{nullptr};
        QLabel* m_statusTargetLabel{nullptr};
        QLabel* m_statusPreviewLabel{nullptr};
        QLabel* m_statusProcessingLabel{nullptr};
        QLabel* m_statusRecordingLabel{nullptr};
        QTimer* m_statusMessageTimer{nullptr};
        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QString m_currentControlTarget{QStringLiteral("All")};
        QPoint m_lastPreviewMousePos{-1, -1};
    };
}
