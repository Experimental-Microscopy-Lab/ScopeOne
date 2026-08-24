#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QWidget>
#include <memory>

class QListWidget;
class QCheckBox;
class QComboBox;
class QPushButton;
class QProgressBar;
class QStackedWidget;

namespace scopeone::ui
{
    class ImageWorkspace;

    class ImageProcessingWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit ImageProcessingWidget(scopeone::core::ScopeOneCore* core,
                                       ImageWorkspace* workspace,
                                       QWidget* parent = nullptr);
        ~ImageProcessingWidget() override = default;

    signals:
        void processingStarted();
        void processingStopped();
        void processedLayerReady(const QString& layerKey);
        void processedStackReady(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);

    private:
        void onAddModuleClicked();
        void onRemoveModuleClicked();
        void onModuleSelectionChanged();
        void onProcessingBitDepthChanged();
        void onLiveProcessingToggled(bool enabled);
        void onRunOfflineProcessing();
        void onCancelProcessing();

        void setupUI();
        void setupRunControls();
        void setupSourceControls();
        QWidget* setupModuleList();
        QWidget* setupModuleConfig();
        void updateProcessingSettings();
        void updateModuleList();
        void updateConfigWidget();
        void updateRunButtons();
        void syncProcessingState();
        void refreshSources();
        void finishOfflineProcessing();

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        bool m_processingRunning{false};
        QWidget* m_runControlsWidget{nullptr};
        QWidget* m_sourceControlsWidget{nullptr};
        QComboBox* m_sourceCombo{nullptr};
        QCheckBox* m_liveProcessingCheckBox{nullptr};
        QPushButton* m_runOfflineButton{nullptr};
        QPushButton* m_cancelProcessingButton{nullptr};
        QProgressBar* m_processingProgress{nullptr};
        QComboBox* m_processingBitDepthCombo{nullptr};
        QComboBox* m_liveSourceCombo{nullptr};
        QComboBox* m_offlineScopeCombo{nullptr};
        QListWidget* m_moduleList{nullptr};
        QPushButton* m_addModuleButton{nullptr};
        QPushButton* m_removeModuleButton{nullptr};
        QComboBox* m_moduleTypeCombo{nullptr};
        QStackedWidget* m_configStack{nullptr};
        QWidget* m_emptyConfigWidget{nullptr};
        quint64 m_offlineProcessingRequestId{0};
        bool m_directProcessingRequest{false};
    };
}
