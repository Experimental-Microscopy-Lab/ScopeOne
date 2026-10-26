#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QJsonArray>
#include <QWidget>
#include <memory>

class QListWidget;
class QListWidgetItem;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QToolButton;
class QMenu;
class QProgressBar;
class QRadioButton;
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

        // Puts a module with the given parameters at the end of the pipeline
        void addModule(const QString& moduleId, const QVariantMap& parameters);

    signals:
        // A problem the user should see, which does not stop anything else
        void problem(const QString& title, const QString& message);
        void processingStarted();
        void processingStopped();
        void processedLayerReady(const QString& layerKey);
        void processedStackReady(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);

    private:
        void onAddModuleClicked();
        void onTargetChanged();
        void onTargetButtonClicked();
        void refreshTargets();
        bool pipelineEditable() const;
        bool anyPipelineHasSteps() const;
        void fillPresetMenu();
        void savePreset();
        void applyPipeline(const QJsonArray& steps);
        QJsonArray currentPipeline() const;
        void onRemoveModuleClicked();
        void onMoveModuleUpClicked();
        void onMoveModuleDownClicked();
        void onModuleItemChanged(QListWidgetItem* item);
        void onModuleSelectionChanged();
        void onProcessingBitDepthChanged();
        void onInputModeChanged();
        void onLiveProcessingToggled(bool enabled);
        void onRunOfflineProcessing();
        void onCancelProcessing();

        void setupUI();
        QWidget* setupInputControls();
        QWidget* setupExecutionControls();
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
        QComboBox* m_sourceCombo{nullptr};
        QRadioButton* m_liveModeRadio{nullptr};
        QRadioButton* m_staticModeRadio{nullptr};
        QLabel* m_sourceLabel{nullptr};
        QLabel* m_rangeLabel{nullptr};
        QPushButton* m_executeButton{nullptr};
        QPushButton* m_cancelProcessingButton{nullptr};
        QProgressBar* m_processingProgress{nullptr};
        QComboBox* m_processingBitDepthCombo{nullptr};
        QComboBox* m_liveSourceCombo{nullptr};
        QComboBox* m_offlineScopeCombo{nullptr};
        QListWidget* m_moduleList{nullptr};
        QPushButton* m_addModuleButton{nullptr};
        QPushButton* m_removeModuleButton{nullptr};
        QToolButton* m_presetButton{nullptr};
        // The camera whose pipeline the list shows, or empty for the shared one that cameras without a pipeline of their own use
        QString m_targetCamera;
        QComboBox* m_targetCombo{nullptr};
        QLabel* m_targetInfo{nullptr};
        QPushButton* m_targetButton{nullptr};
        QMenu* m_presetMenu{nullptr};
        QPushButton* m_moveModuleUpButton{nullptr};
        QPushButton* m_moveModuleDownButton{nullptr};
        QComboBox* m_moduleTypeCombo{nullptr};
        QStackedWidget* m_configStack{nullptr};
        QGroupBox* m_configGroup{nullptr};
        QWidget* m_emptyConfigWidget{nullptr};
        quint64 m_offlineProcessingRequestId{0};
        bool m_directProcessingRequest{false};
    };
}
