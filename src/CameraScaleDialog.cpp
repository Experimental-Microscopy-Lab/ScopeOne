#include "CameraScaleDialog.h"

#include "ScopeOneToolPlugin.h"
#include "scopeone/ImageSceneModel.h"
#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <limits>

namespace scopeone::ui
{
    namespace
    {
        using scopeone::core::ImageFrame;
        using scopeone::core::ScopeOneCore;
    }

    // Create the per camera image scale editor
    CameraScaleDialog::CameraScaleDialog(scopeone::core::ScopeOneCore* core,
                                         QWidget* parent)
        : QDialog(parent)
          , m_core(core)
    {
        if (!core)
        {
            qFatal("CameraScaleDialog requires ScopeOneCore");
        }

        setWindowTitle(tr("Scale"));
        auto* mainLayout = new QVBoxLayout(this);
        auto* formLayout = new QFormLayout();
        m_cameraCombo = new QComboBox(this);
        m_cameraCombo->addItems(m_core->cameraIds());
        formLayout->addRow(tr("Camera"), m_cameraCombo);

        m_pixelSizeSpinBox = new QDoubleSpinBox(this);
        m_pixelSizeSpinBox->setRange(0.0, 1000000.0);
        m_pixelSizeSpinBox->setDecimals(6);
        m_pixelSizeSpinBox->setSingleStep(0.01);
        m_pixelSizeSpinBox->setSuffix(QStringLiteral(" µm/px"));
        m_pixelSizeSpinBox->setSpecialValueText(tr("Unset"));
        m_pixelSizeSpinBox->setKeyboardTracking(false);
        formLayout->addRow(tr("Pixel Size"), m_pixelSizeSpinBox);
        mainLayout->addLayout(formLayout);

        m_statusLabel = new QLabel(tr("Scale applies to the selected camera for this session"), this);
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                this, &CameraScaleDialog::applyScale);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        mainLayout->addWidget(buttons);

        connect(m_cameraCombo, &QComboBox::currentTextChanged,
                this, [this]() { loadSelectedCamera(); });
        loadSelectedCamera();
    }

    // Load the global scale for the selected camera
    void CameraScaleDialog::loadSelectedCamera()
    {
        const QString cameraId = m_cameraCombo->currentText().trimmed();
        m_pixelSizeSpinBox->setEnabled(!cameraId.isEmpty());
        m_pixelSizeSpinBox->setValue(cameraId.isEmpty() ? 0.0 : m_core->cameraPixelSizeUm(cameraId));
    }

    // Apply one camera scale for the current application session
    void CameraScaleDialog::applyScale()
    {
        const QString cameraId = m_cameraCombo->currentText().trimmed();
        const double pixelSizeUm = m_pixelSizeSpinBox->value();
        if (!m_core->setCameraPixelSizeUm(cameraId, pixelSizeUm))
        {
            m_statusLabel->setText(tr("Failed to update camera scale"));
            return;
        }

        if (pixelSizeUm > 0.0)
        {
            m_statusLabel->setText(tr("Scale updated for %1").arg(cameraId));
        }
        else
        {
            m_statusLabel->setText(tr("Scale cleared for %1").arg(cameraId));
        }
    }
}
