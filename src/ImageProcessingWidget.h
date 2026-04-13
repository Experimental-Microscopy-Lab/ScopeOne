#pragma once

#include <QWidget>

class QListWidget;
class QComboBox;
class QPushButton;
class QStackedWidget;

namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui {

class ImageProcessingWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ImageProcessingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~ImageProcessingWidget() override = default;

signals:
    void processingStarted();

private:
    void onAddModuleClicked();
    void onRemoveModuleClicked();
    void onModuleSelectionChanged();
    void onStartProcessing();
    void onStopProcessing();

    void setupUI();
    void setupRunControls();
    void setupModuleList();
    void setupModuleConfig();
    void updateModuleList();
    void updateConfigWidget();
    void updateRunButtons();

    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    QWidget* m_runControlsWidget{nullptr};
    QPushButton* m_startButton{nullptr};
    QPushButton* m_stopButton{nullptr};
    QListWidget* m_moduleList{nullptr};
    QPushButton* m_addModuleButton{nullptr};
    QPushButton* m_removeModuleButton{nullptr};
    QComboBox* m_moduleTypeCombo{nullptr};
    QStackedWidget* m_configStack{nullptr};
    QWidget* m_emptyConfigWidget{nullptr};
};

}
