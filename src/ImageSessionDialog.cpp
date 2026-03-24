#include "ImageSessionDialog.h"

#include "InspectWidget.h"
#include "PreviewWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace {

QString formatFrameInfoText(const QString& cameraId, int frameIndex, int frameCount)
{
    if (cameraId.isEmpty()) {
        return QStringLiteral("No image");
    }
    if (frameCount <= 0) {
        return QStringLiteral("%1 | No frames").arg(cameraId);
    }
    return QStringLiteral("%1 | Frame %2 / %3")
        .arg(cameraId)
        .arg(frameIndex + 1)
        .arg(frameCount);
}

QString formatZoomText(int zoomPercent)
{
    return QStringLiteral("Zoom: %1%").arg(zoomPercent);
}

} // namespace

namespace scopeone::ui {

using scopeone::core::ScopeOneCore;
using scopeone::core::ImageFrame;
using scopeone::core::ImagePixelFormat;

ImageSessionDialog::ImageSessionDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUI();
    rebuildCameraList();
}

ImageSessionDialog::ImageSessionDialog(
    std::shared_ptr<const ScopeOneCore::RecordingSessionData> session,
    QWidget* parent)
    : QDialog(parent)
{
    setupUI();
    setRecordingSession(std::move(session));
}

void ImageSessionDialog::setRecordingSession(
    std::shared_ptr<const ScopeOneCore::RecordingSessionData> session)
{
    m_session = std::move(session);
    rebuildCameraList();
}

void ImageSessionDialog::setLiveFrame(const ImageFrame& frame)
{
    if (!frame.isValid() || frame.cameraId.trimmed().isEmpty()) {
        return;
    }

    m_liveFrames.insert(frame.cameraId, frame);

    const QString currentCameraId = m_cameraCombo ? m_cameraCombo->currentText() : QString();
    if (!m_cameraCombo || m_cameraCombo->findText(frame.cameraId) < 0) {
        rebuildCameraList();
        if (m_cameraCombo) {
            const int index = m_cameraCombo->findText(currentCameraId.isEmpty() ? frame.cameraId : currentCameraId);
            if (index >= 0) {
                m_cameraCombo->setCurrentIndex(index);
            }
        }
    } else if (frame.cameraId == currentCameraId) {
        updateDisplayedFrame();
    }
}

void ImageSessionDialog::clearLiveFrames()
{
    m_liveFrames.clear();
    rebuildCameraList();
}

void ImageSessionDialog::setSaveEnabled(bool enabled)
{
    m_saveEnabledByOwner = enabled;
    if (m_saveButton) {
        m_saveButton->setEnabled(m_saveEnabledByOwner && m_session && currentFrameCount() > 0);
    }
}

void ImageSessionDialog::setSaveButtonText(const QString& text)
{
    if (m_saveButton) {
        m_saveButton->setText(text);
    }
}

void ImageSessionDialog::setupUI()
{
    setWindowTitle(QStringLiteral("Image Session"));
    setMinimumSize(900, 700);

    auto* mainLayout = new QVBoxLayout(this);

    auto* topRow = new QHBoxLayout();
    m_cameraCombo = new QComboBox(this);
    topRow->addWidget(new QLabel(QStringLiteral("Camera:"), this));
    topRow->addWidget(m_cameraCombo, 1);

    m_frameSlider = new QSlider(Qt::Horizontal, this);
    m_frameSlider->setMinimum(0);
    m_frameSlider->setMaximum(0);
    topRow->addWidget(new QLabel(QStringLiteral("Frame:"), this));
    topRow->addWidget(m_frameSlider, 2);

    m_frameSpin = new QSpinBox(this);
    m_frameSpin->setMinimum(0);
    m_frameSpin->setMaximum(0);
    m_frameSpin->setMinimumWidth(80);
    topRow->addWidget(m_frameSpin);
    mainLayout->addLayout(topRow);

    auto* displayRow = new QHBoxLayout();
    m_frameInfoLabel = new QLabel(QStringLiteral("No image"), this);
    displayRow->addWidget(m_frameInfoLabel, 1);

    m_fitToWindowCheck = new QCheckBox(QStringLiteral("Fit"), this);
    m_fitToWindowCheck->setChecked(true);
    displayRow->addWidget(m_fitToWindowCheck);

    m_zoomSlider = new QSlider(Qt::Horizontal, this);
    m_zoomSlider->setRange(10, 500);
    m_zoomSlider->setValue(100);
    m_zoomSlider->setMaximumWidth(160);
    displayRow->addWidget(m_zoomSlider);

    m_zoomLabel = new QLabel(formatZoomText(m_zoomSlider->value()), this);
    displayRow->addWidget(m_zoomLabel);
    mainLayout->addLayout(displayRow);

    auto* contentRow = new QHBoxLayout();
    m_previewWidget = new PreviewWidget(this);
    m_previewWidget->setFitToWindow(true);
    m_previewWidget->setZoomLevel(100);
    contentRow->addWidget(m_previewWidget, 1);

    m_inspectWidget = new InspectWidget(nullptr, this);
    m_inspectWidget->onCameraInitialized(true);
    m_inspectWidget->setMinimumWidth(280);
    m_inspectWidget->setCrossSectionVisible(false);
    contentRow->addWidget(m_inspectWidget);
    mainLayout->addLayout(contentRow, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    m_saveButton = new QPushButton(QStringLiteral("Save"), this);
    m_saveButton->setEnabled(false);
    m_closeButton = new QPushButton(QStringLiteral("Close"), this);
    buttonRow->addWidget(m_saveButton);
    buttonRow->addWidget(m_closeButton);
    mainLayout->addLayout(buttonRow);

    connect(m_cameraCombo, &QComboBox::currentTextChanged, this,
            [this](const QString&) {
                updateFrameControls();
                updateDisplayedFrame();
            });
    connect(m_frameSlider, &QSlider::valueChanged, this,
            [this](int value) {
                if (m_frameSpin->value() != value) {
                    m_frameSpin->setValue(value);
                }
                updateDisplayedFrame();
            });
    connect(m_frameSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int value) {
                if (m_frameSlider->value() != value) {
                    m_frameSlider->setValue(value);
                }
                updateDisplayedFrame();
            });
    connect(m_fitToWindowCheck, &QCheckBox::toggled, this,
            [this](bool checked) {
                if (m_previewWidget) {
                    m_previewWidget->setFitToWindow(checked);
                }
            });
    connect(m_zoomSlider, &QSlider::valueChanged, this,
            [this](int value) {
                m_zoomLabel->setText(formatZoomText(value));
                if (value != 100 && m_fitToWindowCheck->isChecked()) {
                    m_fitToWindowCheck->setChecked(false);
                }
                if (m_previewWidget) {
                    m_previewWidget->setZoomLevel(value);
                }
            });
    connect(m_previewWidget, &PreviewWidget::fitToWindowChanged, this,
            [this](bool enabled) {
                const QSignalBlocker blocker(m_fitToWindowCheck);
                m_fitToWindowCheck->setChecked(enabled);
            });
    connect(m_previewWidget, &PreviewWidget::zoomLevelChanged, this,
            [this](int value) {
                const QSignalBlocker blocker(m_zoomSlider);
                m_zoomSlider->setValue(value);
                m_zoomLabel->setText(formatZoomText(value));
            });
    connect(m_inspectWidget, &InspectWidget::displayRangeChanged, this,
            [this](const QString& cameraId,
                   bool processed,
                   int minLevel,
                   int maxLevel,
                   int maxDisplayValue) {
                if (!m_previewWidget || cameraId.isEmpty()) {
                    return;
                }
                m_previewWidget->setChannelDisplayLevels(cameraId,
                                                         processed,
                                                         minLevel,
                                                         maxLevel,
                                                         maxDisplayValue);
            });
    connect(m_saveButton, &QPushButton::clicked, this,
            [this]() {
                m_saveRequested = true;
                accept();
            });
    connect(m_closeButton, &QPushButton::clicked, this,
            [this]() {
                m_saveRequested = false;
                reject();
            });
    if (m_inspectWidget) {
        m_inspectWidget->clearInspect();
    }
}

void ImageSessionDialog::rebuildCameraList()
{
    // Merge recorded and live cameras into one selector
    QStringList cameraIds;
    if (m_session) {
        for (const QString& cameraId : m_session->recordedCameraIds()) {
            const auto* frames = m_session->framesForCamera(cameraId);
            if (frames && !frames->empty() && !cameraIds.contains(cameraId)) {
                cameraIds.append(cameraId);
            }
        }
    }
    for (auto it = m_liveFrames.constBegin(); it != m_liveFrames.constEnd(); ++it) {
        if (!cameraIds.contains(it.key())) {
            cameraIds.append(it.key());
        }
    }

    const QString currentCameraId = m_cameraCombo ? m_cameraCombo->currentText() : QString();
    const QSignalBlocker blocker(m_cameraCombo);
    m_cameraCombo->clear();
    m_cameraCombo->addItems(cameraIds);

    int nextIndex = -1;
    if (!currentCameraId.isEmpty()) {
        nextIndex = m_cameraCombo->findText(currentCameraId);
    }
    if (nextIndex < 0 && !cameraIds.isEmpty()) {
        nextIndex = 0;
    }
    if (nextIndex >= 0) {
        m_cameraCombo->setCurrentIndex(nextIndex);
    }

    updateFrameControls();
    updateDisplayedFrame();
}

void ImageSessionDialog::updateFrameControls()
{
    // Keep frame controls aligned with the current camera
    const int count = currentFrameCount();
    const int maxIndex = (count > 0) ? count - 1 : 0;

    const QSignalBlocker sliderBlocker(m_frameSlider);
    const QSignalBlocker spinBlocker(m_frameSpin);
    m_frameSlider->setMaximum(maxIndex);
    m_frameSpin->setMaximum(maxIndex);
    if (m_frameSlider->value() > maxIndex) {
        m_frameSlider->setValue(maxIndex);
    }
    if (m_frameSpin->value() > maxIndex) {
        m_frameSpin->setValue(maxIndex);
    }

    const bool hasIndexedFrames = count > 1;
    m_frameSlider->setEnabled(hasIndexedFrames);
    m_frameSpin->setEnabled(hasIndexedFrames);
    m_saveButton->setEnabled(m_saveEnabledByOwner && m_session && count > 0);
}

void ImageSessionDialog::updateDisplayedFrame()
{
    // Prefer recorded frames and fall back to live frames
    if (!m_previewWidget) {
        return;
    }

    const QString cameraId = currentCameraId();
    const int frameCount = currentFrameCount();
    if (cameraId.isEmpty() || frameCount <= 0) {
        if (!m_lastPreviewCameraId.isEmpty()) {
            m_previewWidget->clearChannel(m_lastPreviewCameraId);
            m_lastPreviewCameraId.clear();
        }
        m_frameInfoLabel->setText(QStringLiteral("No image"));
        if (m_inspectWidget) {
            m_inspectWidget->clearInspect();
        }
        return;
    }

    if (m_session) {
        const auto* frames = m_session->framesForCamera(cameraId);
        if (frames && !frames->empty()) {
            const int index = std::clamp(m_frameSlider->value(), 0, static_cast<int>(frames->size()) - 1);
            const auto& frame = (*frames)[static_cast<size_t>(index)];
            showPreviewFrame(normalizedFrame(frame, cameraId));
            m_frameInfoLabel->setText(formatFrameInfoText(cameraId, index, static_cast<int>(frames->size())));
            return;
        }
    }

    const auto liveIt = m_liveFrames.constFind(cameraId);
    if (liveIt != m_liveFrames.constEnd()) {
        showPreviewFrame(liveIt.value());
        m_frameInfoLabel->setText(formatFrameInfoText(cameraId, 0, 1));
        return;
    }

    if (!m_lastPreviewCameraId.isEmpty()) {
        m_previewWidget->clearChannel(m_lastPreviewCameraId);
        m_lastPreviewCameraId.clear();
    }
    m_frameInfoLabel->setText(QStringLiteral("No image"));
    if (m_inspectWidget) {
        m_inspectWidget->clearInspect();
    }
}

int ImageSessionDialog::currentFrameCount() const
{
    const QString cameraId = currentCameraId();
    if (cameraId.isEmpty()) {
        return 0;
    }

    if (m_session) {
        const auto* frames = m_session->framesForCamera(cameraId);
        if (frames) {
            return static_cast<int>(frames->size());
        }
    }

    return m_liveFrames.contains(cameraId) ? 1 : 0;
}

QString ImageSessionDialog::currentCameraId() const
{
    return m_cameraCombo ? m_cameraCombo->currentText().trimmed() : QString();
}

ImageFrame ImageSessionDialog::normalizedFrame(const ScopeOneCore::RecordingFrame& frame,
                                               const QString& cameraId) const
{
    // Convert stored recording data into a preview frame
    ImageFrame normalized;
    normalized.cameraId = cameraId;
    normalized.width = (frame.width > 0) ? frame.width : static_cast<int>(frame.header.width);
    normalized.height = (frame.height > 0) ? frame.height : static_cast<int>(frame.header.height);
    normalized.bitsPerSample = (frame.bits > 0) ? frame.bits : static_cast<int>(frame.header.bitsPerSample);
    if (normalized.bitsPerSample <= 0) {
        normalized.bitsPerSample = 8;
    }
    normalized.pixelFormat = (normalized.bitsPerSample > 8)
        ? ImagePixelFormat::Mono16
        : ImagePixelFormat::Mono8;
    const int bytesPerPixel = normalized.bytesPerPixel();
    normalized.stride = (frame.header.stride > 0)
        ? static_cast<int>(frame.header.stride)
        : normalized.width * bytesPerPixel;
    normalized.bytes = frame.rawData;
    return normalized;
}

void ImageSessionDialog::showPreviewFrame(const ImageFrame& frame)
{
    // Show one frame and refresh its histogram view
    if (!frame.isValid() || frame.cameraId.isEmpty()) {
        return;
    }

    if (!m_lastPreviewCameraId.isEmpty() && m_lastPreviewCameraId != frame.cameraId) {
        m_previewWidget->clearChannel(m_lastPreviewCameraId);
    }

    m_previewWidget->setAvailableChannels({frame.cameraId});
    m_previewWidget->setSelectedChannels({QStringLiteral("raw:%1").arg(frame.cameraId)});
    m_previewWidget->setChannelRaw(frame);
    m_lastPreviewCameraId = frame.cameraId;
    ScopeOneCore::HistogramStats stats;
    if (!ScopeOneCore::computeHistogramStats(frame, stats) || !stats.hasData()) {
        if (m_inspectWidget) {
            m_inspectWidget->clearInspect();
        }
        return;
    }
    if (m_inspectWidget) {
        m_inspectWidget->setFrameInspect(frame.cameraId, stats);
    }
}

} // namespace scopeone::ui
