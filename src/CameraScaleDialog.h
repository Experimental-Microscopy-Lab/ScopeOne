#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace scopeone::ui
{
    class CameraScaleDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit CameraScaleDialog(scopeone::core::ScopeOneCore* core,
                                   QWidget* parent = nullptr);

    private:
        void loadSelectedCamera();
        void applyScale();

        scopeone::core::ScopeOneCore* m_core{nullptr};
        QComboBox* m_cameraCombo{nullptr};
        QDoubleSpinBox* m_pixelSizeSpinBox{nullptr};
        QLabel* m_statusLabel{nullptr};
    };
}
