#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QPoint>
#include <QStringList>

class QAction;
class QDockWidget;
class QMenu;
class QProgressDialog;

namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui {

class DevicePropertyWidget;
class ImageProcessingWidget;
class InspectWidget;
class PreviewWidget;
class ConsoleWidget;
class DeviceControlWidget;
class RecordingWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    void setupUI();
    void setupSignalWiring();

    void setupMenuBar();
    void setupDeviceControl();
    void setupInspect();
    void setupImageProcessing();
    void setupConsole();
    void setupPropertyBrowser();
    void setupRecording();

    void closeLoadConfigProgress();
    void updateControlTarget(const QString& target);
    void updateDockWidgetMenu();
    void applyLoadedCameraState(const QStringList& cameraIds);
    void applyUnloadedCameraState(const QStringList& cameraIds);
    void refreshDevicePanels(bool fromCache = false);
    void applyStoredApplicationSettings();
    void openSettingsDialog();

    void handlePreviewMousePosition(const QPoint& pos);
    void handleRoiDrawn(const QString& cameraId, int x, int y, int width, int height);
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

    QDockWidget* m_recordingDockWidget{nullptr};
    RecordingWidget* m_recordingWidget{nullptr};

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
    QAction* m_aboutAction{nullptr};
    QAction* m_aboutQtAction{nullptr};

    QPointer<QProgressDialog> m_loadConfigProgress;
    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    QString m_currentControlTarget{QStringLiteral("All")};
};

}
