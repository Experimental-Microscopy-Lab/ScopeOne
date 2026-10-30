#include "StageMosaicDialog.h"

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

        // Returns the save directory shared with the recording panel
        QString defaultGallerySaveDirectory()
        {
            QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
            const QString saveDir = settings.value(QStringLiteral("LastSaveDirectory"),
                                                   QDir::homePath()).toString();
            return QDir(saveDir).exists() ? saveDir : QDir::homePath();
        }
    }

    // Create a stage driven mosaic tool
    StageMosaicDialog::StageMosaicDialog(ScopeOneToolContext& context,
                                         QWidget* parent)
        : QDialog(parent)
          , m_core(&context.core())
          , m_context(context)
    {
        setWindowTitle(tr("Stage Mosaic"));
        setupUI();
        refreshDevices();
        connect(m_core, &ScopeOneCore::stageMosaicProgress,
                this, [this](int, int, const QString& message)
                {
                    m_statusLabel->setText(message);
                });
        connect(m_core, &ScopeOneCore::stageMosaicFinished,
                this, [this](const std::shared_ptr<ScopeOneCore::RecordingSessionData>& session,
                             const QString& message,
                             bool)
                {
                    setMosaicRunning(false);
                    if (!session)
                    {
                        m_context.showToolStatus(message, 8000);
                        return;
                    }
                    m_context.presentSession(
                        session, tr("Stage Mosaic %1").arg(session->cameraIds().value(0)));
                    m_core->removeStaticFrame(QStringLiteral("stage_mosaic"));
                });
        connect(m_core, &ScopeOneCore::stageMosaicFrameUpdated,
                this, [this](const scopeone::core::ImageFrame&)
                {
                    const QString layerKey = ScopeOneCore::staticLayerKey(
                        QStringLiteral("stage_mosaic"));
                    m_core->imageSceneModel()->setLayerColormap(layerKey, QStringLiteral("Gray"));
                    m_core->imageSceneModel()->setLayerBlending(layerKey, QStringLiteral("Opaque"));
                    m_context.showLayers({layerKey});
                });
        const ScopeOneCore::StageMosaicStatus status = m_core->stageMosaicStatus();
        if (status.state == ScopeOneCore::StageMosaicState::Running)
        {
            setMosaicRunning(true);
            m_statusLabel->setText(status.message);
        }
    }

    // Stop active capture before closing the dialog
    void StageMosaicDialog::reject()
    {
        m_core->cancelStageMosaic();
        QDialog::reject();
    }

    // Build the mosaic control form
    void StageMosaicDialog::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);

        auto* captureGroup = new QGroupBox(tr("Capture"), this);
        auto* captureLayout = new QFormLayout(captureGroup);

        m_cameraCombo = new QComboBox(captureGroup);
        m_stageCombo = new QComboBox(captureGroup);
        m_rowsSpinBox = new QSpinBox(captureGroup);
        m_rowsSpinBox->setRange(1, 100);
        m_rowsSpinBox->setValue(3);
        m_columnsSpinBox = new QSpinBox(captureGroup);
        m_columnsSpinBox->setRange(1, 100);
        m_columnsSpinBox->setValue(3);

        m_stepXSpinBox = new QDoubleSpinBox(captureGroup);
        m_stepXSpinBox->setRange(-1000000.0, 1000000.0);
        m_stepXSpinBox->setDecimals(3);
        m_stepXSpinBox->setValue(100.0);
        m_stepXSpinBox->setSuffix(QStringLiteral(" um"));
        m_stepXSpinBox->setKeyboardTracking(false);

        m_stepYSpinBox = new QDoubleSpinBox(captureGroup);
        m_stepYSpinBox->setRange(-1000000.0, 1000000.0);
        m_stepYSpinBox->setDecimals(3);
        m_stepYSpinBox->setValue(100.0);
        m_stepYSpinBox->setSuffix(QStringLiteral(" um"));
        m_stepYSpinBox->setKeyboardTracking(false);

        m_settleMsSpinBox = new QSpinBox(captureGroup);
        m_settleMsSpinBox->setRange(0, 10000);
        m_settleMsSpinBox->setValue(150);
        m_settleMsSpinBox->setSuffix(QStringLiteral(" ms"));
        m_settleMsSpinBox->setKeyboardTracking(false);

        m_returnToStartCheckBox = new QCheckBox(tr("Return to start"), captureGroup);
        m_returnToStartCheckBox->setChecked(true);

        captureLayout->addRow(tr("Camera"), m_cameraCombo);
        captureLayout->addRow(tr("XY Stage"), m_stageCombo);
        captureLayout->addRow(tr("Rows"), m_rowsSpinBox);
        captureLayout->addRow(tr("Columns"), m_columnsSpinBox);
        captureLayout->addRow(tr("Step X"), m_stepXSpinBox);
        captureLayout->addRow(tr("Step Y"), m_stepYSpinBox);
        captureLayout->addRow(tr("Settle"), m_settleMsSpinBox);
        captureLayout->addRow(QString(), m_returnToStartCheckBox);
        mainLayout->addWidget(captureGroup);

        auto* buttonLayout = new QGridLayout();
        m_startButton = new QPushButton(tr("Start"), this);
        m_stopButton = new QPushButton(tr("Stop"), this);
        m_stopButton->setEnabled(false);
        buttonLayout->addWidget(m_startButton, 0, 0);
        buttonLayout->addWidget(m_stopButton, 0, 1);
        mainLayout->addLayout(buttonLayout);

        m_statusLabel = new QLabel(tr("Ready"), this);
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(closeButtons, &QDialogButtonBox::rejected, this, &StageMosaicDialog::reject);
        mainLayout->addWidget(closeButtons);

        connect(m_startButton, &QPushButton::clicked, this, &StageMosaicDialog::startMosaic);
        connect(m_stopButton, &QPushButton::clicked, this, &StageMosaicDialog::stopMosaic);
    }

    // Refresh cameras and XY stages from the core
    void StageMosaicDialog::refreshDevices()
    {
        m_cameraCombo->clear();
        m_stageCombo->clear();

        m_cameraCombo->addItems(m_core->cameraIds());
        m_stageCombo->addItems(m_core->xyStageDevices());

        const QString currentStage = m_core->currentXYStageDevice();
        const int stageIndex = m_stageCombo->findText(currentStage);
        if (stageIndex >= 0)
        {
            m_stageCombo->setCurrentIndex(stageIndex);
        }
        const bool ready = m_cameraCombo->count() > 0 && m_stageCombo->count() > 0;
        m_startButton->setEnabled(ready);
        if (!ready)
        {
            m_statusLabel->setText(tr("Select a camera and XY stage"));
        }
    }

    QString StageMosaicDialog::selectedCameraId() const
    {
        return m_cameraCombo->currentText().trimmed();
    }

    QString StageMosaicDialog::selectedStageId() const
    {
        return m_stageCombo->currentText().trimmed();
    }

    // Start a grid mosaic from the current stage position
    void StageMosaicDialog::startMosaic()
    {
        const QString activeCameraId = selectedCameraId();
        ScopeOneCore::StageMosaicPlan plan;
        plan.cameraId = activeCameraId;
        plan.xyStageId = selectedStageId();
        plan.rows = m_rowsSpinBox->value();
        plan.columns = m_columnsSpinBox->value();
        plan.pixelSizeUm = m_core->cameraPixelSizeUm(activeCameraId);
        plan.stepXUm = m_stepXSpinBox->value();
        plan.stepYUm = m_stepYSpinBox->value();
        plan.settleMs = m_settleMsSpinBox->value();
        plan.returnToStart = m_returnToStartCheckBox->isChecked();
        plan.gallerySaveDir = defaultGallerySaveDirectory();
        if (plan.cameraId.isEmpty() || plan.xyStageId.isEmpty())
        {
            m_statusLabel->setText(tr("Select a camera and XY stage"));
            return;
        }
        if (plan.pixelSizeUm <= 0.0)
        {
            m_statusLabel->setText(tr("Set the camera scale in Tools > Scale"));
            return;
        }

        QString errorMessage;
        if (!m_core->startStageMosaic(plan, &errorMessage))
        {
            m_statusLabel->setText(errorMessage);
            return;
        }
        setMosaicRunning(true);
        m_statusLabel->setText(tr("Starting mosaic capture"));
        m_context.showLayers({scopeone::core::ScopeOneCore::rawLayerKey(activeCameraId)});
    }

    // Request cancellation after the current stage move
    void StageMosaicDialog::stopMosaic()
    {
        m_core->cancelStageMosaic();
    }

    void StageMosaicDialog::setMosaicRunning(bool running)
    {
        m_startButton->setEnabled(!running && m_cameraCombo->count() > 0 && m_stageCombo->count() > 0);
        m_stopButton->setEnabled(running);
        m_cameraCombo->setEnabled(!running);
        m_stageCombo->setEnabled(!running);
        m_rowsSpinBox->setEnabled(!running);
        m_columnsSpinBox->setEnabled(!running);
        m_stepXSpinBox->setEnabled(!running);
        m_stepYSpinBox->setEnabled(!running);
        m_settleMsSpinBox->setEnabled(!running);
        m_returnToStartCheckBox->setEnabled(!running);
    }
}
