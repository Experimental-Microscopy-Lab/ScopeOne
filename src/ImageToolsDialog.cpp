#include "ImageToolsDialog.h"

#include "PreviewWidget.h"
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
    StageMosaicDialog::StageMosaicDialog(scopeone::core::ScopeOneCore* core,
                                         PreviewWidget* previewWidget,
                                         QWidget* parent)
        : QDialog(parent)
          , m_core(core)
          , m_previewWidget(previewWidget)
    {
        if (!core || !previewWidget)
        {
            qFatal("StageMosaicDialog requires ScopeOneCore and PreviewWidget");
        }

        setWindowTitle(tr("Stage Mosaic"));
        setupUI();
        refreshDevices();
        connect(m_core, &ScopeOneCore::stageMosaicProgress,
                this, [this](int, int, const QString& message)
                {
                    m_statusLabel->setText(message);
                });
        connect(m_core, &ScopeOneCore::stageMosaicFinished,
                this, [this](const std::shared_ptr<ScopeOneCore::RecordingSessionData>&,
                             const QString&,
                             bool)
                {
                    setMosaicRunning(false);
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

        m_pixelSizeSpinBox = new QDoubleSpinBox(captureGroup);
        m_pixelSizeSpinBox->setRange(0.001, 1000000.0);
        m_pixelSizeSpinBox->setDecimals(4);
        m_pixelSizeSpinBox->setValue(1.0);
        m_pixelSizeSpinBox->setSuffix(QStringLiteral(" um/px"));
        m_pixelSizeSpinBox->setKeyboardTracking(false);

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
        captureLayout->addRow(tr("Pixel Size"), m_pixelSizeSpinBox);
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
        m_activeCameraId = selectedCameraId();
        ScopeOneCore::StageMosaicPlan plan;
        plan.cameraId = m_activeCameraId;
        plan.xyStageId = selectedStageId();
        plan.rows = m_rowsSpinBox->value();
        plan.columns = m_columnsSpinBox->value();
        plan.pixelSizeUm = m_pixelSizeSpinBox->value();
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

        QString errorMessage;
        if (!m_core->startStageMosaic(plan, &errorMessage))
        {
            m_statusLabel->setText(errorMessage);
            return;
        }
        setMosaicRunning(true);
        m_statusLabel->setText(tr("Starting mosaic capture"));
        m_core->imageSceneModel()->setVisibleLayers(
            {scopeone::core::ScopeOneCore::rawLayerKey(m_activeCameraId)});
        m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
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
        m_pixelSizeSpinBox->setEnabled(!running);
        m_stepXSpinBox->setEnabled(!running);
        m_stepYSpinBox->setEnabled(!running);
        m_settleMsSpinBox->setEnabled(!running);
        m_returnToStartCheckBox->setEnabled(!running);
    }

    // Create a particle detection tool
    ParticleDetectionDialog::ParticleDetectionDialog(scopeone::core::ScopeOneCore* core,
                                                     PreviewWidget* previewWidget,
                                                     QWidget* parent)
        : QDialog(parent)
          , m_core(core)
          , m_previewWidget(previewWidget)
    {
        if (!core || !previewWidget)
        {
            qFatal("ParticleDetectionDialog requires ScopeOneCore and PreviewWidget");
        }

        setWindowTitle(tr("Particle Detection"));
        setupUI();
        refreshCameras();
    }

    // Stop automatic analysis before closing
    void ParticleDetectionDialog::reject()
    {
        m_autoUpdateTimer->stop();
        QDialog::reject();
    }

    // Build controls for threshold based particle detection
    void ParticleDetectionDialog::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        auto* formLayout = new QFormLayout();

        m_cameraCombo = new QComboBox(this);
        m_thresholdSpinBox = new QSpinBox(this);
        m_thresholdSpinBox->setRange(0, 65535);
        m_thresholdSpinBox->setValue(128);
        m_thresholdSpinBox->setKeyboardTracking(false);

        m_minAreaSpinBox = new QSpinBox(this);
        m_minAreaSpinBox->setRange(1, 100000000);
        m_minAreaSpinBox->setValue(5);
        m_minAreaSpinBox->setKeyboardTracking(false);

        m_maxAreaSpinBox = new QSpinBox(this);
        m_maxAreaSpinBox->setRange(1, 100000000);
        m_maxAreaSpinBox->setValue(100000000);
        m_maxAreaSpinBox->setKeyboardTracking(false);

        formLayout->addRow(tr("Camera"), m_cameraCombo);
        formLayout->addRow(tr("Threshold"), m_thresholdSpinBox);
        formLayout->addRow(tr("Min Area"), m_minAreaSpinBox);
        formLayout->addRow(tr("Max Area"), m_maxAreaSpinBox);
        mainLayout->addLayout(formLayout);

        m_analyzeButton = new QPushButton(tr("Analyze Latest Frame"), this);
        mainLayout->addWidget(m_analyzeButton);

        m_autoUpdateCheckBox = new QCheckBox(tr("Auto Update"), this);
        mainLayout->addWidget(m_autoUpdateCheckBox);

        m_statusLabel = new QLabel(tr("Ready"), this);
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(closeButtons, &QDialogButtonBox::rejected, this, &ParticleDetectionDialog::reject);
        mainLayout->addWidget(closeButtons);

        m_autoUpdateTimer = new QTimer(this);
        m_autoUpdateTimer->setInterval(500);

        connect(m_analyzeButton, &QPushButton::clicked, this, &ParticleDetectionDialog::analyzeCurrentFrame);
        connect(m_autoUpdateCheckBox, &QCheckBox::toggled, this, [this](bool enabled)
        {
            if (enabled)
            {
                m_autoUpdateTimer->start();
                analyzeCurrentFrame();
                return;
            }
            m_autoUpdateTimer->stop();
        });
        connect(m_autoUpdateTimer, &QTimer::timeout, this, &ParticleDetectionDialog::analyzeCurrentFrame);
    }

    // Refresh available cameras
    void ParticleDetectionDialog::refreshCameras()
    {
        m_cameraCombo->clear();
        m_cameraCombo->addItems(m_core->cameraIds());
        m_analyzeButton->setEnabled(m_cameraCombo->count() > 0);
        m_autoUpdateCheckBox->setEnabled(m_cameraCombo->count() > 0);
        if (m_cameraCombo->count() == 0)
        {
            m_statusLabel->setText(tr("Select a camera"));
        }
    }

    QString ParticleDetectionDialog::selectedCameraId() const
    {
        return m_cameraCombo->currentText().trimmed();
    }

    // Request preview once when the tool needs a fresh camera frame
    void ParticleDetectionDialog::ensurePreviewRunning(const QString& cameraId)
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty() || m_requestedPreviewCameraId == trimmedCameraId)
        {
            return;
        }

        m_core->startPreview(trimmedCameraId);
        m_requestedPreviewCameraId = trimmedCameraId;
    }

    // Detect connected bright particles in the latest frame
    void ParticleDetectionDialog::analyzeCurrentFrame()
    {
        const QString cameraId = selectedCameraId();
        if (cameraId.isEmpty())
        {
            m_statusLabel->setText(tr("Select a camera"));
            return;
        }
        ensurePreviewRunning(cameraId);

        const ImageFrame frame = m_core->graphFrame(ScopeOneCore::rawLayerKey(cameraId));
        if (!frame.isValid())
        {
            m_statusLabel->setText(tr("Waiting for live frame"));
            return;
        }
        const int maxPixelValue = frame.maxValue();
        if (m_thresholdSpinBox->maximum() != maxPixelValue)
        {
            QSignalBlocker blocker(m_thresholdSpinBox);
            m_thresholdSpinBox->setMaximum(maxPixelValue);
        }
        const qint64 framePixelCount = static_cast<qint64>(frame.width) * frame.height;
        const int maxParticleArea = static_cast<int>(
            qBound<qint64>(1,
                           framePixelCount,
                           static_cast<qint64>((std::numeric_limits<int>::max)())));
        if (m_minAreaSpinBox->maximum() != maxParticleArea)
        {
            QSignalBlocker minBlocker(m_minAreaSpinBox);
            QSignalBlocker maxBlocker(m_maxAreaSpinBox);
            m_minAreaSpinBox->setMaximum(maxParticleArea);
            m_maxAreaSpinBox->setMaximum(maxParticleArea);
        }

        const int threshold = qBound(0, m_thresholdSpinBox->value(), maxPixelValue);
        const int minArea = m_minAreaSpinBox->value();
        if (m_maxAreaSpinBox->value() < minArea)
        {
            QSignalBlocker blocker(m_maxAreaSpinBox);
            m_maxAreaSpinBox->setValue(minArea);
        }
        const int maxArea = m_maxAreaSpinBox->value();
        ScopeOneCore::ParticleDetectionResult result;
        if (!m_core->detectParticles(ScopeOneCore::rawLayerKey(cameraId),
                                     threshold,
                                     minArea,
                                     maxArea,
                                     result))
        {
            m_statusLabel->setText(tr("Particle analysis failed"));
            return;
        }
        const QString maskLayerId = QStringLiteral("particle_mask");
        const ImageFrame previewMaskFrame = m_core->publishStaticFrame(
            maskLayerId,
            result.mask,
            tr("Particles %1").arg(cameraId));
        if (!previewMaskFrame.isValid())
        {
            m_statusLabel->setText(tr("Failed to display particle mask"));
            return;
        }
        const QString maskLayer = scopeone::core::ScopeOneCore::staticLayerKey(maskLayerId);

        m_core->imageSceneModel()->setLayerColormap(maskLayer, QStringLiteral("Magenta"));
        m_core->imageSceneModel()->setLayerOpacityPercent(maskLayer, 70);
        m_core->imageSceneModel()->setLayerBlending(maskLayer, QStringLiteral("Additive"));
        m_core->imageSceneModel()->setLayerVisible(
            scopeone::core::ScopeOneCore::rawLayerKey(cameraId), true);
        m_core->imageSceneModel()->setVisibleLayers(
            {scopeone::core::ScopeOneCore::rawLayerKey(cameraId), maskLayer});
        m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
        m_statusLabel->setText(
            result.truncated
                ? tr("Detected at least %1 particle(s)").arg(result.particles.size())
                : tr("Detected %1 particle(s)").arg(result.particles.size()));
    }
}
