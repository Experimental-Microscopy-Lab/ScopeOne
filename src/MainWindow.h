#pragma once

#include "scopeone/ScopeOneCore.h"
#include "ScopeOneToolPlugin.h"

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
class QTabWidget;
class QTimer;

namespace scopeone::core
{
    class ImageSceneModel;
    class ScopeOneCore;
}

namespace scopeone::ui
{
    class DevicePropertyWidget;
    class ConfigPresetWidget;
    class ImageGalleryWidget;
    class ImageWorkspace;
    class ImageProcessingWidget;
    class InspectWidget;
    class PreviewWidget;
    class ConsoleWidget;
    class DeviceControlWidget;
    class RecordingWidget;
    class MainWindow : public QMainWindow, public ScopeOneToolContext
    {
        Q_OBJECT

    public:
        explicit MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~MainWindow() override = default;

        scopeone::core::ScopeOneCore& core() const override;
        QString currentLayerKey() const override;
        void showLayers(const QStringList& layerKeys, bool sideBySide = false) override;
        void showToolStatus(const QString& message, int timeoutMs = 5000) override;
        void presentSession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& title) override;

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        void setupUI();
        void setupSignalWiring();
        void setupStatusBar();
        void setupTools();

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
        void applyNoCameraState();
        void refreshDevicePanels(bool fromCache = false);
        void applyStoredApplicationSettings();
        void logStartupSummary();
        void openSettingsDialog();
        void connectPropertyPanels();
        void showStatusMessage(const QString& message, int timeoutMs = 0);
        void setCursorStatus(const QString& text);
        void clearCursorStatus();
        void refreshPreviewCursorStatus();
        void schedulePreviewCursorStatusRefresh();
        void showMeasurementLine(const QString& layerKey,
                                 const QPoint& start,
                                 const QPoint& end);
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
                                               const QString& errorMessage);
        void handleCloseSaveFinished(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        void loadConfigurationFromDialog();
        void loadConfigurationPath(const QString& configPath);
        void refreshRecentConfigurationsMenu();
        void unloadConfigurationWithConfirmation();
        void setFullScreenEnabled(bool enabled);

        scopeone::core::ImageSceneModel* m_imageSceneModel{nullptr};
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
        ImageWorkspace* m_imageWorkspace{nullptr};

        DeviceControlWidget* m_deviceControlWidget{nullptr};

        ImageProcessingWidget* m_imageProcessingWidget{nullptr};

        QDockWidget* m_inspectorDockWidget{nullptr};
        QTabWidget* m_inspectorTabs{nullptr};
        InspectWidget* m_inspectWidget{nullptr};

        QMenu* m_fileMenu{nullptr};
        QMenu* m_recentConfigurationsMenu{nullptr};
        QMenu* m_viewMenu{nullptr};
        QMenu* m_toolsMenu{nullptr};
        QMenu* m_helpMenu{nullptr};
        QMenu* m_dockWidgetsMenu{nullptr};

        QAction* m_exitAction{nullptr};
        QAction* m_fullScreenAction{nullptr};
        QAction* m_loadConfigurationAction{nullptr};
        QAction* m_unloadConfigurationAction{nullptr};
        QAction* m_saveImageAsAction{nullptr};
        QAction* m_settingsAction{nullptr};
        QAction* m_aboutAction{nullptr};
        QAction* m_aboutQtAction{nullptr};

        QPointer<QProgressDialog> m_loadConfigProgress;
        QPointer<QProgressDialog> m_closeSaveProgress;
        std::unique_ptr<ToolRegistry> m_toolRegistry;
        QLabel* m_statusMessageLabel{nullptr};
        QLabel* m_statusCursorLabel{nullptr};
        QLabel* m_statusPreviewLabel{nullptr};
        QLabel* m_statusProcessingLabel{nullptr};
        QLabel* m_statusRecordingLabel{nullptr};
        QTimer* m_statusMessageTimer{nullptr};
        QTimer* m_cursorRefreshTimer{nullptr};
        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QString m_currentControlTarget{QStringLiteral("All")};
        QPoint m_lastPreviewMousePos{-1, -1};
        QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> m_closePendingSessions;
        int m_closeSaveTotal{0};
        bool m_closeSaveInProgress{false};
        bool m_closeAfterSave{false};
    };
}
