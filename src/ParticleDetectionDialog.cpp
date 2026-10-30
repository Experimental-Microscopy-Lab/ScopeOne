#include "ParticleDetectionDialog.h"

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

    // Create a particle detection tool
    ParticleDetectionDialog::ParticleDetectionDialog(ScopeOneToolContext& context,
                                                     QWidget* parent)
        : QDialog(parent)
          , m_core(&context.core())
          , m_context(context)
    {
        setWindowTitle(tr("Particle Detection"));
        setupUI();
        connect(m_core, &ScopeOneCore::particleDetectionFinished,
                this, &ParticleDetectionDialog::handleDetectionFinished);
        refreshCameras();
    }

    // Stop automatic analysis before closing
    void ParticleDetectionDialog::reject()
    {
        m_autoUpdateTimer->stop();
        m_activeDetectionRequestId = 0;
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
        if (m_activeDetectionRequestId != 0)
        {
            return;
        }
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
        m_activeDetectionRequestId = m_core->detectParticles(
            ScopeOneCore::rawLayerKey(cameraId), threshold, minArea, maxArea);
        if (m_activeDetectionRequestId == 0)
        {
            m_statusLabel->setText(tr("Particle analysis failed"));
        }
        else
        {
            m_analyzeButton->setEnabled(false);
            m_statusLabel->setText(tr("Analyzing..."));
        }
    }

    // Publishes the latest particle result on the UI thread
    void ParticleDetectionDialog::handleDetectionFinished(
        quint64 requestId,
        const QString& layerKey,
        const ScopeOneCore::ParticleDetectionResult& result,
        const QString& errorMessage)
    {
        if (requestId != m_activeDetectionRequestId)
        {
            return;
        }
        m_activeDetectionRequestId = 0;
        m_analyzeButton->setEnabled(m_cameraCombo->count() > 0);
        if (!errorMessage.isEmpty())
        {
            m_statusLabel->setText(errorMessage);
            return;
        }

        const QString cameraId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
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
        m_context.showLayers(
            {scopeone::core::ScopeOneCore::rawLayerKey(cameraId), maskLayer});
        m_statusLabel->setText(
            result.truncated
                ? tr("Detected at least %1 particle(s)").arg(result.particles.size())
                : tr("Detected %1 particle(s)").arg(result.particles.size()));
    }
}
