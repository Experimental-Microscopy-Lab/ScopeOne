#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace scopeone::ui
{
    class ScopeOneToolContext;

    class StageMosaicDialog : public QDialog
    {
        Q_OBJECT

    public:
        StageMosaicDialog(scopeone::core::ScopeOneCore* core,
                          ScopeOneToolContext& context,
                          QWidget* parent = nullptr);

        void reject() override;

    private:
        void setupUI();
        void refreshDevices();
        void startMosaic();
        void stopMosaic();
        void setMosaicRunning(bool running);
        QString selectedCameraId() const;
        QString selectedStageId() const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        ScopeOneToolContext& m_context;
        QComboBox* m_cameraCombo{nullptr};
        QComboBox* m_stageCombo{nullptr};
        QSpinBox* m_rowsSpinBox{nullptr};
        QSpinBox* m_columnsSpinBox{nullptr};
        QDoubleSpinBox* m_stepXSpinBox{nullptr};
        QDoubleSpinBox* m_stepYSpinBox{nullptr};
        QSpinBox* m_settleMsSpinBox{nullptr};
        QCheckBox* m_returnToStartCheckBox{nullptr};
        QPushButton* m_startButton{nullptr};
        QPushButton* m_stopButton{nullptr};
        QLabel* m_statusLabel{nullptr};
    };
}
