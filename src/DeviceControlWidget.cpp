#include "DeviceControlWidget.h"
#include "scopeone/ScopeOneCore.h"
#include "PreviewWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleValidator>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace scopeone::ui
{
    namespace
    {
        int streamLayoutComboIndex(PreviewWidget::StreamLayoutMode mode)
        {
            return mode == PreviewWidget::StreamLayoutMode::Overlay ? 1 : 0;
        }

        PreviewWidget::StreamLayoutMode streamLayoutModeFromComboIndex(int index)
        {
            return index == 1
                       ? PreviewWidget::StreamLayoutMode::Overlay
                       : PreviewWidget::StreamLayoutMode::SideBySide;
        }
    } // namespace

    DeviceControlWidget::DeviceControlWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
          , m_cameraInitialized(false)
          , m_previewRunning(false)
          , m_currentTarget("All")
    {
        if (!m_scopeonecore)
        {
            qFatal("DeviceControlWidget requires ScopeOneCore");
        }

        setWindowTitle("Control");

        setupUI();
        updateControlsState();
        refreshStageDevices();
        if (m_cameraSelectCombo)
        {
            m_currentTarget = m_cameraSelectCombo->currentText();
        }
    }

    void DeviceControlWidget::setupUI()
    {
        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setSpacing(0);
        mainLayout->setContentsMargins(0, 0, 0, 0);

        QScrollArea* scrollArea = new QScrollArea();
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setFrameShape(QFrame::NoFrame);

        QWidget* contentContainer = new QWidget();
        QVBoxLayout* contentLayout = new QVBoxLayout(contentContainer);
        contentLayout->setSpacing(5);
        contentLayout->setContentsMargins(5, 5, 5, 5);

        contentLayout->addWidget(createPreviewControlsGroup());

        contentLayout->addWidget(createControlGroup());
        contentLayout->addWidget(createStageGroup());

        contentLayout->addStretch();

        scrollArea->setWidget(contentContainer);
        mainLayout->addWidget(scrollArea);
    }

    void DeviceControlWidget::setPreviewWidget(PreviewWidget* preview)
    {
        if (m_previewWidget == preview)
        {
            return;
        }
        if (m_previewWidget)
        {
            disconnect(m_previewWidget, nullptr, this, nullptr);
        }
        m_previewWidget = preview;
        if (!m_previewWidget)
        {
            return;
        }

        connect(m_previewWidget, &PreviewWidget::availableCameraIdsChanged,
                this, &DeviceControlWidget::onPreviewAvailableCameraIdsChanged);
        connect(m_previewWidget, &PreviewWidget::selectedStreamsChanged,
                this, [this](const QStringList& streamKeys)
                {
                    applyPreviewSelection(streamKeys, false);
                });
        connect(m_previewWidget, &PreviewWidget::streamLayoutModeChanged,
                this, [this](PreviewWidget::StreamLayoutMode mode)
                {
                    syncPreviewStreamLayoutCombo(streamLayoutComboIndex(mode));
                });
        connect(m_previewWidget, &PreviewWidget::cameraInfoTextChanged,
                this, &DeviceControlWidget::onPreviewInfoTextChanged);
        connect(m_previewWidget, &PreviewWidget::zoomLevelChanged, this,
                [this](int value)
                {
                    if (m_zoomSpinBox)
                    {
                        const QSignalBlocker blocker(m_zoomSpinBox);
                        m_zoomSpinBox->setValue(value);
                    }
                });
        connect(m_previewWidget, &PreviewWidget::fitToWindowChanged, this,
                [this](bool enabled)
                {
                    if (m_fitToWindowCheckBox)
                    {
                        const QSignalBlocker blocker(m_fitToWindowCheckBox);
                        m_fitToWindowCheckBox->setChecked(enabled);
                    }
                    updatePreviewZoomControls();
                });

        onPreviewAvailableCameraIdsChanged(m_previewWidget->availableCameraIds());
        applyPreviewSelection(m_previewWidget->selectedStreams(), false);
        syncPreviewStreamLayoutCombo(streamLayoutComboIndex(m_previewWidget->streamLayoutMode()));
        onPreviewInfoTextChanged(m_previewWidget->cameraInfoText());

        if (m_zoomSpinBox)
        {
            QSignalBlocker blocker(m_zoomSpinBox);
            m_zoomSpinBox->setValue(m_previewWidget->zoomPercent());
        }
        if (m_fitToWindowCheckBox)
        {
            QSignalBlocker blocker(m_fitToWindowCheckBox);
            m_fitToWindowCheckBox->setChecked(m_previewWidget->isFitToWindow());
        }
        if (m_overlayAlphaSpinBox)
        {
            QSignalBlocker blocker(m_overlayAlphaSpinBox);
            m_overlayAlphaSpinBox->setValue(m_previewWidget->overlayAlphaPercent());
        }
        updatePreviewZoomControls();
    }

    QWidget* DeviceControlWidget::createPreviewControlsGroup()
    {
        m_previewControlsGroup = new QGroupBox("Preview Controls", this);
        QGridLayout* controlLayout = new QGridLayout(m_previewControlsGroup);
        controlLayout->setHorizontalSpacing(6);
        controlLayout->setVerticalSpacing(4);
        controlLayout->setContentsMargins(6, 6, 6, 6);

        m_zoomLabel = new QLabel("Zoom:", this);
        m_zoomSpinBox = new QSpinBox(this);
        m_zoomSpinBox->setRange(10, 500);
        m_zoomSpinBox->setValue(100);
        m_zoomSpinBox->setSuffix("%");
        m_zoomSpinBox->setMinimumWidth(70);

        m_fitToWindowCheckBox = new QCheckBox("Fit to Window", this);
        m_fitToWindowCheckBox->setChecked(true);

        m_streamLayoutCombo = new QComboBox(this);
        m_streamLayoutCombo->addItem("Side-by-side");
        m_streamLayoutCombo->addItem("Overlay");

        m_overlayAlphaSpinBox = new QSpinBox(this);
        m_overlayAlphaSpinBox->setRange(0, 100);
        m_overlayAlphaSpinBox->setValue(50);
        m_overlayAlphaSpinBox->setSuffix("%");
        m_overlayAlphaSpinBox->setMinimumWidth(60);

        m_streamPickerButton = new QToolButton(this);
        m_streamPickerButton->setText("Select");
        m_streamPickerButton->setPopupMode(QToolButton::InstantPopup);
        m_streamMenu = new QMenu(this);
        populatePreviewStreamMenuHeader();
        m_streamPickerButton->setMenu(m_streamMenu);

        m_alignLabel = new QLabel("Align:", this);
        m_alignCameraCombo = new QComboBox(this);
        m_alignCameraCombo->setMinimumWidth(80);

        m_alignXLabel = new QLabel("X offset:", this);
        m_alignXSpinBox = new QSpinBox(this);
        m_alignXSpinBox->setRange(-1000, 1000);
        m_alignXSpinBox->setValue(0);
        m_alignXSpinBox->setMinimumWidth(60);

        m_alignYLabel = new QLabel("Y offset:", this);
        m_alignYSpinBox = new QSpinBox(this);
        m_alignYSpinBox->setRange(-1000, 1000);
        m_alignYSpinBox->setValue(0);
        m_alignYSpinBox->setMinimumWidth(60);

        m_alignZoomLabel = new QLabel("Zoom:", this);
        m_alignZoomSpinBox = new QSpinBox(this);
        m_alignZoomSpinBox->setRange(10, 500);
        m_alignZoomSpinBox->setValue(100);
        m_alignZoomSpinBox->setMinimumWidth(60);
        m_alignZoomSpinBox->setToolTip("Per-camera zoom percent");

        m_alignFlipXCheckBox = new QCheckBox("Flip X", this);
        m_alignFlipYCheckBox = new QCheckBox("Flip Y", this);
        m_alignResetButton = new QPushButton("Reset", this);
        m_alignResetButton->setMaximumWidth(50);
        m_alignResetButton->setToolTip("Reset offset and flip");

        m_imageInfoLabel = new QLabel("No image loaded", this);
        m_imageInfoLabel->setStyleSheet("QLabel { color: #666; font-size: 11px; }");
        m_imageInfoLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
        m_imageInfoLabel->setWordWrap(false);

        m_zoomLabel->setMinimumWidth(60);
        m_alignLabel->setMinimumWidth(60);
        m_alignXLabel->setMinimumWidth(20);
        m_alignYLabel->setMinimumWidth(20);
        m_alignZoomLabel->setMinimumWidth(60);

        QLabel* streamsLabel = new QLabel("Streams:", this);
        QLabel* alphaLabel = new QLabel("Alpha:", this);
        streamsLabel->setMinimumWidth(60);
        alphaLabel->setMinimumWidth(60);

        controlLayout->addWidget(m_zoomLabel, 0, 0);
        controlLayout->addWidget(m_zoomSpinBox, 0, 1);
        controlLayout->addWidget(m_fitToWindowCheckBox, 0, 2, 1, 2);

        controlLayout->addWidget(streamsLabel, 1, 0);
        controlLayout->addWidget(m_streamLayoutCombo, 1, 1);
        controlLayout->addWidget(alphaLabel, 1, 2);
        controlLayout->addWidget(m_overlayAlphaSpinBox, 1, 3);
        controlLayout->addWidget(m_streamPickerButton, 1, 4);

        controlLayout->addWidget(m_alignLabel, 2, 0);
        controlLayout->addWidget(m_alignCameraCombo, 2, 1);
        controlLayout->addWidget(m_alignXLabel, 2, 2);
        controlLayout->addWidget(m_alignXSpinBox, 2, 3);
        controlLayout->addWidget(m_alignYLabel, 2, 4);
        controlLayout->addWidget(m_alignYSpinBox, 2, 5);

        controlLayout->addWidget(m_alignZoomLabel, 3, 0);
        controlLayout->addWidget(m_alignZoomSpinBox, 3, 1);
        controlLayout->addWidget(m_alignFlipXCheckBox, 3, 2);
        controlLayout->addWidget(m_alignFlipYCheckBox, 3, 3);
        controlLayout->addWidget(m_alignResetButton, 3, 4);

        controlLayout->addWidget(m_imageInfoLabel, 4, 0, 1, 6);

        controlLayout->setColumnStretch(1, 1);
        controlLayout->setColumnStretch(3, 1);
        controlLayout->setColumnStretch(5, 1);

        connect(m_zoomSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewZoomSpinBoxChanged);
        connect(m_fitToWindowCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onPreviewFitToWindowToggled);
        connect(m_streamLayoutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewStreamLayoutComboChanged);
        connect(m_overlayAlphaSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewOverlayAlphaChanged);

        connect(m_alignCameraCombo, &QComboBox::currentTextChanged,
                this, &DeviceControlWidget::onAlignCameraChanged);
        connect(m_alignXSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onAlignXChanged);
        connect(m_alignYSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onAlignYChanged);
        connect(m_alignZoomSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onAlignZoomChanged);
        connect(m_alignFlipXCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onAlignFlipXChanged);
        connect(m_alignFlipYCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onAlignFlipYChanged);
        connect(m_alignResetButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onAlignResetClicked);

        updatePreviewZoomControls();
        return m_previewControlsGroup;
    }

    void DeviceControlWidget::updatePreviewZoomControls()
    {
        if (!m_zoomSpinBox || !m_fitToWindowCheckBox)
        {
            return;
        }
        m_zoomSpinBox->setEnabled(!m_fitToWindowCheckBox->isChecked());
    }

    void DeviceControlWidget::rebuildPreviewStreamMenu(const QStringList& cameraIds)
    {
        if (!m_streamMenu)
        {
            return;
        }

        m_streamMenu->clear();
        m_streamActions.clear();
        populatePreviewStreamMenuHeader();

        for (const QString& id : cameraIds)
        {
            QString keyRaw = QString("raw:%1").arg(id);
            QAction* actRaw = new QAction(QString("%1 (Raw)").arg(id), m_streamMenu);
            actRaw->setCheckable(true);
            actRaw->setData(keyRaw);
            connect(actRaw, &QAction::toggled, this, &DeviceControlWidget::onPreviewStreamActionToggled);
            m_streamMenu->addAction(actRaw);
            m_streamActions.insert(keyRaw, actRaw);

            QString keyProc = QString("proc:%1").arg(id);
            QAction* actProc = new QAction(QString("%1 (Processed)").arg(id), m_streamMenu);
            actProc->setCheckable(true);
            actProc->setData(keyProc);
            connect(actProc, &QAction::toggled, this, &DeviceControlWidget::onPreviewStreamActionToggled);
            m_streamMenu->addAction(actProc);
            m_streamActions.insert(keyProc, actProc);
        }
    }

    void DeviceControlWidget::updatePreviewSelectionFromActions()
    {
        QStringList selectedStreamKeys;
        for (auto it = m_streamActions.begin(); it != m_streamActions.end(); ++it)
        {
            if (it.value()->isChecked())
            {
                selectedStreamKeys.append(it.key());
            }
        }

        if (m_previewWidget)
        {
            m_previewWidget->setSelectedStreams(selectedStreamKeys);
        }
    }

    void DeviceControlWidget::populatePreviewStreamMenuHeader()
    {
        if (!m_streamMenu)
        {
            return;
        }

        QAction* selAllAct = m_streamMenu->addAction("Select All");
        connect(selAllAct, &QAction::triggered, this, &DeviceControlWidget::onSelectAllPreviewStreams);
        QAction* selAllRawAct = m_streamMenu->addAction("Select All Raw");
        connect(selAllRawAct, &QAction::triggered, this, &DeviceControlWidget::onSelectAllPreviewRaw);
        QAction* selAllProcAct = m_streamMenu->addAction("Select All Processed");
        connect(selAllProcAct, &QAction::triggered, this, &DeviceControlWidget::onSelectAllPreviewProcessed);
        QAction* clrSelAct = m_streamMenu->addAction("Clear Selection");
        connect(clrSelAct, &QAction::triggered, this, &DeviceControlWidget::onClearPreviewSelection);
        m_streamMenu->addSeparator();
    }

    void DeviceControlWidget::applyPreviewSelection(const QStringList& streamKeys, bool notifyPreview)
    {
        const QSet<QString> selectedStreamKeys(streamKeys.begin(), streamKeys.end());
        for (auto it = m_streamActions.begin(); it != m_streamActions.end(); ++it)
        {
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(selectedStreamKeys.contains(it.key()));
        }

        if (notifyPreview && m_previewWidget)
        {
            m_previewWidget->setSelectedStreams(streamKeys);
        }
    }

    void DeviceControlWidget::setPreviewStreamActionStates(const QString& selectedPrefix,
                                                           bool checkedWhenPrefixEmpty)
    {
        for (auto it = m_streamActions.begin(); it != m_streamActions.end(); ++it)
        {
            const bool checked = selectedPrefix.isEmpty()
                                     ? checkedWhenPrefixEmpty
                                     : it.key().startsWith(selectedPrefix);
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(checked);
        }
        updatePreviewSelectionFromActions();
    }

    void DeviceControlWidget::onPreviewAvailableCameraIdsChanged(const QStringList& cameraIds)
    {
        rebuildPreviewStreamMenu(cameraIds);

        if (m_alignCameraCombo)
        {
            QString currentSelection = m_alignCameraCombo->currentText();
            QSignalBlocker blocker(m_alignCameraCombo);
            m_alignCameraCombo->clear();
            m_alignCameraCombo->addItems(cameraIds);

            int idx = m_alignCameraCombo->findText(currentSelection);
            if (idx >= 0)
            {
                m_alignCameraCombo->setCurrentIndex(idx);
            }
            else if (!cameraIds.isEmpty())
            {
                m_alignCameraCombo->setCurrentIndex(0);
                onAlignCameraChanged(cameraIds.first());
            }
        }
    }

    void DeviceControlWidget::syncPreviewStreamLayoutCombo(int index)
    {
        if (!m_streamLayoutCombo)
        {
            return;
        }
        QSignalBlocker blocker(m_streamLayoutCombo);
        m_streamLayoutCombo->setCurrentIndex(index);
    }

    void DeviceControlWidget::onPreviewInfoTextChanged(const QString& text)
    {
        if (m_imageInfoLabel)
        {
            m_imageInfoLabel->setText(text);
        }
    }

    void DeviceControlWidget::onPreviewZoomSpinBoxChanged(int value)
    {
        if (m_previewWidget)
        {
            m_previewWidget->setZoomPercent(value);
        }
    }

    void DeviceControlWidget::onPreviewFitToWindowToggled(bool enabled)
    {
        if (m_previewWidget)
        {
            m_previewWidget->setFitToWindow(enabled);
        }
        updatePreviewZoomControls();
    }

    void DeviceControlWidget::onPreviewStreamLayoutComboChanged(int index)
    {
        if (m_previewWidget)
        {
            m_previewWidget->setStreamLayoutMode(streamLayoutModeFromComboIndex(index));
        }
    }

    void DeviceControlWidget::onPreviewOverlayAlphaChanged(int value)
    {
        if (m_previewWidget)
        {
            m_previewWidget->setOverlayAlphaPercent(value);
        }
    }

    void DeviceControlWidget::onPreviewStreamActionToggled(bool)
    {
        updatePreviewSelectionFromActions();
    }

    void DeviceControlWidget::onSelectAllPreviewStreams()
    {
        setPreviewStreamActionStates(QString(), true);
    }

    void DeviceControlWidget::onClearPreviewSelection()
    {
        setPreviewStreamActionStates(QString(), false);
    }

    void DeviceControlWidget::onSelectAllPreviewRaw()
    {
        setPreviewStreamActionStates(QStringLiteral("raw:"), false);
    }

    void DeviceControlWidget::onSelectAllPreviewProcessed()
    {
        setPreviewStreamActionStates(QStringLiteral("proc:"), false);
    }

    void DeviceControlWidget::onAlignCameraChanged(const QString& cameraId)
    {
        if (cameraId.isEmpty())
        {
            return;
        }

        if (m_alignXSpinBox && m_alignYSpinBox)
        {
            QSignalBlocker bx(*m_alignXSpinBox);
            QSignalBlocker by(*m_alignYSpinBox);
            m_alignXSpinBox->setValue(0);
            m_alignYSpinBox->setValue(0);
        }

        if (m_alignZoomSpinBox)
        {
            QSignalBlocker bz(*m_alignZoomSpinBox);
            m_alignZoomSpinBox->setValue(100);
        }

        if (m_alignFlipXCheckBox && m_alignFlipYCheckBox)
        {
            QSignalBlocker bfx(*m_alignFlipXCheckBox);
            QSignalBlocker bfy(*m_alignFlipYCheckBox);
            m_alignFlipXCheckBox->setChecked(false);
            m_alignFlipYCheckBox->setChecked(false);
        }

        if (m_previewWidget)
        {
            m_previewWidget->setCameraOffset(cameraId, 0, 0);
            m_previewWidget->setCameraFlip(cameraId, false, false);
            m_previewWidget->setCameraZoomPercent(cameraId, 100);
        }
    }

    void DeviceControlWidget::onAlignXChanged(int x)
    {
        if (!m_previewWidget || !m_alignCameraCombo || !m_alignYSpinBox)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();
        if (cameraId.isEmpty())
        {
            return;
        }
        const int y = m_alignYSpinBox->value();
        m_previewWidget->setCameraOffset(cameraId, x, y);
    }

    void DeviceControlWidget::onAlignYChanged(int y)
    {
        if (!m_previewWidget || !m_alignCameraCombo || !m_alignXSpinBox)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();
        if (cameraId.isEmpty())
        {
            return;
        }
        const int x = m_alignXSpinBox->value();
        m_previewWidget->setCameraOffset(cameraId, x, y);
    }

    void DeviceControlWidget::onAlignZoomChanged(int percent)
    {
        if (!m_previewWidget || !m_alignCameraCombo)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();
        if (cameraId.isEmpty())
        {
            return;
        }
        m_previewWidget->setCameraZoomPercent(cameraId, percent);
    }

    void DeviceControlWidget::onAlignFlipXChanged(bool enabled)
    {
        if (!m_previewWidget || !m_alignCameraCombo || !m_alignFlipYCheckBox)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();
        if (cameraId.isEmpty())
        {
            return;
        }
        m_previewWidget->setCameraFlip(cameraId, enabled, m_alignFlipYCheckBox->isChecked());
    }

    void DeviceControlWidget::onAlignFlipYChanged(bool enabled)
    {
        if (!m_previewWidget || !m_alignCameraCombo || !m_alignFlipXCheckBox)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();
        if (cameraId.isEmpty())
        {
            return;
        }
        m_previewWidget->setCameraFlip(cameraId, m_alignFlipXCheckBox->isChecked(), enabled);
    }

    void DeviceControlWidget::onAlignResetClicked()
    {
        if (!m_alignCameraCombo)
        {
            return;
        }
        const QString cameraId = m_alignCameraCombo->currentText();

        if (m_alignXSpinBox && m_alignYSpinBox)
        {
            QSignalBlocker bx(*m_alignXSpinBox);
            QSignalBlocker by(*m_alignYSpinBox);
            m_alignXSpinBox->setValue(0);
            m_alignYSpinBox->setValue(0);
        }
        if (m_alignZoomSpinBox)
        {
            QSignalBlocker bz(*m_alignZoomSpinBox);
            m_alignZoomSpinBox->setValue(100);
        }
        if (m_alignFlipXCheckBox && m_alignFlipYCheckBox)
        {
            QSignalBlocker bfx(*m_alignFlipXCheckBox);
            QSignalBlocker bfy(*m_alignFlipYCheckBox);
            m_alignFlipXCheckBox->setChecked(false);
            m_alignFlipYCheckBox->setChecked(false);
        }

        if (!cameraId.isEmpty() && m_previewWidget)
        {
            m_previewWidget->setCameraOffset(cameraId, 0, 0);
            m_previewWidget->setCameraFlip(cameraId, false, false);
            m_previewWidget->setCameraZoomPercent(cameraId, 100);
        }
    }

    QWidget* DeviceControlWidget::createControlGroup()
    {
        QGroupBox* group = new QGroupBox("Camera Controls");
        QGridLayout* layout = new QGridLayout(group);

        int row = 0;

        layout->addWidget(new QLabel("Control Target:"), row, 0);
        m_cameraSelectCombo = new QComboBox();
        m_cameraSelectCombo->addItem("All");
        connect(m_cameraSelectCombo, &QComboBox::currentTextChanged,
                this, &DeviceControlWidget::onControlTargetSelectionChanged);
        layout->addWidget(m_cameraSelectCombo, row, 1);
        row++;

        layout->addWidget(new QLabel("Exposure (ms):"), row, 0);
        m_exposureLineEdit = new QLineEdit();
        auto* exposureValidator = new QDoubleValidator(0.1, 10000.0, 1, m_exposureLineEdit);
        exposureValidator->setNotation(QDoubleValidator::StandardNotation);
        m_exposureLineEdit->setValidator(exposureValidator);
        m_exposureLineEdit->setText(QStringLiteral("100.0"));
        connect(m_exposureLineEdit, &QLineEdit::returnPressed,
                this, &DeviceControlWidget::onExposureChanged);
        layout->addWidget(m_exposureLineEdit, row, 1);
        row++;

        m_previewToggleButton = new QPushButton("Start Preview");
        m_previewToggleButton->setMinimumWidth(140);
        m_previewToggleButton->setMinimumHeight(30);
        connect(m_previewToggleButton, &QPushButton::clicked, this, &DeviceControlWidget::onPreviewToggleClicked);

        layout->addWidget(m_previewToggleButton, row, 0, 1, 2);

        m_drawROIButton = new QPushButton("Draw ROI", group);
        m_clearROIButton = new QPushButton("Clear ROI", group);
        layout->addWidget(m_drawROIButton, ++row, 0);
        layout->addWidget(m_clearROIButton, row, 1);
        connect(m_drawROIButton, &QPushButton::clicked, this, &DeviceControlWidget::onDrawROIClicked);
        connect(m_clearROIButton, &QPushButton::clicked, this, &DeviceControlWidget::onClearROIClicked);

        return group;
    }

    QWidget* DeviceControlWidget::createStageGroup()
    {
        QGroupBox* group = new QGroupBox("Stage Controls");
        QHBoxLayout* mainLayout = new QHBoxLayout(group);
        QVBoxLayout* xyColumn = new QVBoxLayout();
        QVBoxLayout* zColumn = new QVBoxLayout();

        QHBoxLayout* xyDeviceLayout = new QHBoxLayout();
        xyDeviceLayout->addWidget(new QLabel("XY Device:"));
        m_xyStageCombo = new QComboBox();
        m_xyStageCombo->setMinimumWidth(50);
        xyDeviceLayout->addWidget(m_xyStageCombo, 1);
        xyColumn->addLayout(xyDeviceLayout);

        QGridLayout* xyStepLayout = new QGridLayout();
        xyStepLayout->addWidget(new QLabel("> (um):"), 0, 0);
        m_xyStepLineEdit = new QLineEdit(QStringLiteral("10.0"));
        m_xyStepLineEdit->setMinimumWidth(80);
        {
            auto* validator = new QDoubleValidator(0.0, 100000.0, 5, m_xyStepLineEdit);
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            m_xyStepLineEdit->setValidator(validator);
        }
        xyStepLayout->addWidget(m_xyStepLineEdit, 0, 1);

        xyStepLayout->addWidget(new QLabel(">> (um):"), 1, 0);
        m_xyBigStepLineEdit = new QLineEdit(QStringLiteral("100.0"));
        m_xyBigStepLineEdit->setMinimumWidth(80);
        {
            auto* validator = new QDoubleValidator(0.0, 100000.0, 5, m_xyBigStepLineEdit);
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            m_xyBigStepLineEdit->setValidator(validator);
        }
        xyStepLayout->addWidget(m_xyBigStepLineEdit, 1, 1);
        xyColumn->addLayout(xyStepLayout);

        QHBoxLayout* xyControlLayout = new QHBoxLayout();
        QGridLayout* xyPad = new QGridLayout();
        xyPad->setHorizontalSpacing(2);
        xyPad->setVerticalSpacing(2);
        m_xyUpButton = new QPushButton(QStringLiteral("↑"));
        m_xyDownButton = new QPushButton(QStringLiteral("↓"));
        m_xyLeftButton = new QPushButton(QStringLiteral("←"));
        m_xyRightButton = new QPushButton(QStringLiteral("→"));
        m_xyBigUpButton = new QPushButton(QStringLiteral("⇑"));
        m_xyBigDownButton = new QPushButton(QStringLiteral("⇓"));
        m_xyBigLeftButton = new QPushButton(QStringLiteral("⇐"));
        m_xyBigRightButton = new QPushButton(QStringLiteral("⇒"));
        auto setArrowSize = [](QPushButton* button)
        {
            if (button)
            {
                button->setFixedSize(28, 28);
            }
        };
        setArrowSize(m_xyUpButton);
        setArrowSize(m_xyDownButton);
        setArrowSize(m_xyLeftButton);
        setArrowSize(m_xyRightButton);
        setArrowSize(m_xyBigUpButton);
        setArrowSize(m_xyBigDownButton);
        setArrowSize(m_xyBigLeftButton);
        setArrowSize(m_xyBigRightButton);
        QWidget* xyCenterWidget = new QWidget();
        QVBoxLayout* xyCenterLayout = new QVBoxLayout(xyCenterWidget);
        xyCenterLayout->setContentsMargins(0, 0, 0, 0);
        xyCenterLayout->setSpacing(2);
        m_xPosLabel = new QLabel("X: N/A");
        m_yPosLabel = new QLabel("Y: N/A");
        m_xPosLabel->setAlignment(Qt::AlignCenter);
        m_yPosLabel->setAlignment(Qt::AlignCenter);
        xyCenterLayout->addWidget(m_xPosLabel);
        xyCenterLayout->addWidget(m_yPosLabel);
        xyPad->addWidget(m_xyBigUpButton, 0, 2);
        xyPad->addWidget(m_xyUpButton, 1, 2);
        xyPad->addWidget(m_xyBigLeftButton, 2, 0);
        xyPad->addWidget(m_xyLeftButton, 2, 1);
        xyPad->addWidget(xyCenterWidget, 2, 2);
        xyPad->addWidget(m_xyRightButton, 2, 3);
        xyPad->addWidget(m_xyBigRightButton, 2, 4);
        xyPad->addWidget(m_xyDownButton, 3, 2);
        xyPad->addWidget(m_xyBigDownButton, 4, 2);

        xyControlLayout->addStretch();
        xyControlLayout->addLayout(xyPad);
        xyControlLayout->addStretch();
        xyColumn->addLayout(xyControlLayout);
        xyColumn->addStretch();

        QHBoxLayout* zDeviceLayout = new QHBoxLayout();
        zDeviceLayout->addWidget(new QLabel("Z Device:"));
        m_zStageCombo = new QComboBox();
        m_zStageCombo->setMinimumWidth(50);
        zDeviceLayout->addWidget(m_zStageCombo, 1);
        zColumn->addLayout(zDeviceLayout);

        QGridLayout* zStepLayout = new QGridLayout();
        zStepLayout->addWidget(new QLabel("> (um):"), 0, 0);
        m_zStepLineEdit = new QLineEdit(QStringLiteral("1.0"));
        m_zStepLineEdit->setMinimumWidth(80);
        {
            auto* validator = new QDoubleValidator(0.0, 100000.0, 5, m_zStepLineEdit);
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            m_zStepLineEdit->setValidator(validator);
        }
        zStepLayout->addWidget(m_zStepLineEdit, 0, 1);

        zStepLayout->addWidget(new QLabel(">> (um):"), 1, 0);
        m_zBigStepLineEdit = new QLineEdit(QStringLiteral("10.0"));
        m_zBigStepLineEdit->setMinimumWidth(80);
        {
            auto* validator = new QDoubleValidator(0.0, 100000.0, 5, m_zBigStepLineEdit);
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            m_zBigStepLineEdit->setValidator(validator);
        }
        zStepLayout->addWidget(m_zBigStepLineEdit, 1, 1);
        zColumn->addLayout(zStepLayout);

        QHBoxLayout* zControlLayout = new QHBoxLayout();
        QVBoxLayout* zButtonsLayout = new QVBoxLayout();
        m_zUpButton = new QPushButton(QStringLiteral("↑"));
        m_zDownButton = new QPushButton(QStringLiteral("↓"));
        m_zBigUpButton = new QPushButton(QStringLiteral("⇑"));
        m_zBigDownButton = new QPushButton(QStringLiteral("⇓"));
        setArrowSize(m_zUpButton);
        setArrowSize(m_zDownButton);
        setArrowSize(m_zBigUpButton);
        setArrowSize(m_zBigDownButton);
        zButtonsLayout->addWidget(m_zBigUpButton);
        zButtonsLayout->addWidget(m_zUpButton);
        m_zPosLabel = new QLabel("Z: N/A");
        m_zPosLabel->setAlignment(Qt::AlignCenter);
        zButtonsLayout->addWidget(m_zPosLabel);
        zButtonsLayout->addWidget(m_zDownButton);
        zButtonsLayout->addWidget(m_zBigDownButton);
        zControlLayout->addStretch();
        zControlLayout->addLayout(zButtonsLayout);
        zControlLayout->addStretch();
        zColumn->addLayout(zControlLayout);
        zColumn->addStretch();

        mainLayout->addLayout(xyColumn, 1);
        mainLayout->addLayout(zColumn, 1);

        connect(m_xyStageCombo, &QComboBox::currentTextChanged, this, [this]()
        {
            updateStageControlsEnabled();
            updateStagePositions();
        });
        connect(m_zStageCombo, &QComboBox::currentTextChanged, this, [this]()
        {
            updateStageControlsEnabled();
            updateStagePositions();
        });
        const auto moveXYWithStep = [this](QLineEdit* lineEdit, double dxScale, double dyScale)
        {
            const double stepValue = lineEdit ? lineEdit->text().toDouble() : 0.0;
            if (stepValue <= 0.0) return;
            moveXYStage(dxScale * stepValue, dyScale * stepValue);
        };
        const auto moveZWithStep = [this](QLineEdit* lineEdit, double scale)
        {
            const double stepValue = lineEdit ? lineEdit->text().toDouble() : 0.0;
            if (stepValue <= 0.0) return;
            moveZStage(scale * stepValue);
        };
        connect(m_xyUpButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyStepLineEdit, 0.0, 1.0);
        });
        connect(m_xyDownButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyStepLineEdit, 0.0, -1.0);
        });
        connect(m_xyLeftButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyStepLineEdit, -1.0, 0.0);
        });
        connect(m_xyRightButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyStepLineEdit, 1.0, 0.0);
        });
        connect(m_xyBigUpButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyBigStepLineEdit, 0.0, 1.0);
        });
        connect(m_xyBigDownButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyBigStepLineEdit, 0.0, -1.0);
        });
        connect(m_xyBigLeftButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyBigStepLineEdit, -1.0, 0.0);
        });
        connect(m_xyBigRightButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(m_xyBigStepLineEdit, 1.0, 0.0);
        });
        connect(m_zUpButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(m_zStepLineEdit, 1.0);
        });
        connect(m_zDownButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(m_zStepLineEdit, -1.0);
        });
        connect(m_zBigUpButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(m_zBigStepLineEdit, 1.0);
        });
        connect(m_zBigDownButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(m_zBigStepLineEdit, -1.0);
        });

        return group;
    }

    void DeviceControlWidget::refreshStageDevices()
    {
        if (!m_xyStageCombo || !m_zStageCombo)
        {
            return;
        }

        QSignalBlocker blockXY(m_xyStageCombo);
        QSignalBlocker blockZ(m_zStageCombo);
        m_xyStageCombo->clear();
        m_zStageCombo->clear();

        if (!m_scopeonecore->hasCore())
        {
            updateStageControlsEnabled();
            updateStagePositions();
            return;
        }

        const QStringList xyDevices = m_scopeonecore->xyStageDevices();
        for (const QString& dev : xyDevices)
        {
            m_xyStageCombo->addItem(dev);
        }

        const QStringList zDevices = m_scopeonecore->zStageDevices();
        for (const QString& dev : zDevices)
        {
            m_zStageCombo->addItem(dev);
        }

        const QString preferredXY = m_scopeonecore->currentXYStageDevice();
        if (!preferredXY.isEmpty())
        {
            const int idx = m_xyStageCombo->findText(preferredXY);
            if (idx >= 0)
            {
                m_xyStageCombo->setCurrentIndex(idx);
            }
        }
        if (m_xyStageCombo->currentIndex() < 0 && m_xyStageCombo->count() > 0)
        {
            m_xyStageCombo->setCurrentIndex(0);
        }

        const QString preferredZ = m_scopeonecore->currentFocusDevice();
        if (!preferredZ.isEmpty())
        {
            const int idx = m_zStageCombo->findText(preferredZ);
            if (idx >= 0)
            {
                m_zStageCombo->setCurrentIndex(idx);
            }
        }
        if (m_zStageCombo->currentIndex() < 0 && m_zStageCombo->count() > 0)
        {
            m_zStageCombo->setCurrentIndex(0);
        }

        updateStageControlsEnabled();
        updateStagePositions();
    }

    void DeviceControlWidget::updateStageControlsEnabled()
    {
        const bool hasCore = m_scopeonecore->hasCore();
        const bool hasXY = hasCore && !selectedXYStageLabel().isEmpty();
        const bool hasZ = hasCore && !selectedZStageLabel().isEmpty();

        if (m_xyStageCombo) m_xyStageCombo->setEnabled(hasCore && m_xyStageCombo->count() > 0);
        if (m_zStageCombo) m_zStageCombo->setEnabled(hasCore && m_zStageCombo->count() > 0);
        if (m_xyStepLineEdit) m_xyStepLineEdit->setEnabled(hasXY);
        if (m_xyBigStepLineEdit) m_xyBigStepLineEdit->setEnabled(hasXY);
        if (m_zStepLineEdit) m_zStepLineEdit->setEnabled(hasZ);
        if (m_zBigStepLineEdit) m_zBigStepLineEdit->setEnabled(hasZ);
        if (m_xyUpButton) m_xyUpButton->setEnabled(hasXY);
        if (m_xyDownButton) m_xyDownButton->setEnabled(hasXY);
        if (m_xyLeftButton) m_xyLeftButton->setEnabled(hasXY);
        if (m_xyRightButton) m_xyRightButton->setEnabled(hasXY);
        if (m_xyBigUpButton) m_xyBigUpButton->setEnabled(hasXY);
        if (m_xyBigDownButton) m_xyBigDownButton->setEnabled(hasXY);
        if (m_xyBigLeftButton) m_xyBigLeftButton->setEnabled(hasXY);
        if (m_xyBigRightButton) m_xyBigRightButton->setEnabled(hasXY);
        if (m_zUpButton) m_zUpButton->setEnabled(hasZ);
        if (m_zDownButton) m_zDownButton->setEnabled(hasZ);
        if (m_zBigUpButton) m_zBigUpButton->setEnabled(hasZ);
        if (m_zBigDownButton) m_zBigDownButton->setEnabled(hasZ);
    }

    void DeviceControlWidget::updateStagePositions()
    {
        if (!m_xPosLabel || !m_yPosLabel || !m_zPosLabel)
        {
            return;
        }

        if (!m_scopeonecore->hasCore())
        {
            m_xPosLabel->setText("X: N/A");
            m_yPosLabel->setText("Y: N/A");
            m_zPosLabel->setText("Z: N/A");
            return;
        }

        const QString xyLabel = selectedXYStageLabel();
        if (xyLabel.isEmpty())
        {
            m_xPosLabel->setText("X: N/A");
            m_yPosLabel->setText("Y: N/A");
        }
        else
        {
            double x = 0.0;
            double y = 0.0;
            if (m_scopeonecore->readXYPosition(xyLabel, x, y))
            {
                m_xPosLabel->setText(QString("X: %1").arg(QString::number(x, 'g', 17)));
                m_yPosLabel->setText(QString("Y: %1").arg(QString::number(y, 'g', 17)));
            }
            else
            {
                m_xPosLabel->setText("X: N/A");
                m_yPosLabel->setText("Y: N/A");
            }
        }

        const QString zLabel = selectedZStageLabel();
        if (zLabel.isEmpty())
        {
            m_zPosLabel->setText("Z: N/A");
        }
        else
        {
            double z = 0.0;
            if (m_scopeonecore->readZPosition(zLabel, z))
            {
                m_zPosLabel->setText(QString("Z: %1").arg(QString::number(z, 'g', 17)));
            }
            else
            {
                m_zPosLabel->setText("Z: N/A");
            }
        }
    }

    QString DeviceControlWidget::selectedXYStageLabel() const
    {
        if (!m_xyStageCombo || m_xyStageCombo->count() == 0)
        {
            return QString();
        }
        return m_xyStageCombo->currentText().trimmed();
    }

    QString DeviceControlWidget::selectedZStageLabel() const
    {
        if (!m_zStageCombo || m_zStageCombo->count() == 0)
        {
            return QString();
        }
        return m_zStageCombo->currentText().trimmed();
    }

    void DeviceControlWidget::moveXYStage(double dx, double dy)
    {
        if (!m_scopeonecore->hasCore())
        {
            return;
        }
        const QString xyLabel = selectedXYStageLabel();
        if (xyLabel.isEmpty())
        {
            return;
        }
        if (m_scopeonecore->moveXYRelative(xyLabel, dx, dy))
        {
            updateStagePositions();
        }
        else
        {
            qWarning().noquote() << QString("Failed to move XY stage: %1").arg(xyLabel);
        }
    }

    void DeviceControlWidget::moveZStage(double dz)
    {
        if (!m_scopeonecore->hasCore())
        {
            return;
        }
        const QString zLabel = selectedZStageLabel();
        if (zLabel.isEmpty())
        {
            return;
        }
        if (m_scopeonecore->moveZRelative(zLabel, dz))
        {
            updateStagePositions();
        }
        else
        {
            qWarning().noquote() << QString("Failed to move Z stage: %1").arg(zLabel);
        }
    }

    void DeviceControlWidget::onCameraInitialized(bool initialized)
    {
        m_cameraInitialized = initialized;
        updateControlsState();

        if (initialized && m_scopeonecore->hasCore())
        {
            updateCameraParametersFromHardware();
        }
    }

    void DeviceControlWidget::onExposureChanged()
    {
        // Apply exposure on Enter
        if (!m_exposureLineEdit)
        {
            return;
        }

        bool ok = false;
        const double exposureMs = m_exposureLineEdit->text().trimmed().toDouble(&ok);
        if (!ok || exposureMs < 0.1 || exposureMs > 10000.0)
        {
            updateCameraParametersFromHardware();
            return;
        }

        m_exposureLineEdit->setText(QString::number(exposureMs, 'f', 1));
        emit exposureValueChanged(exposureMs);
    }

    void DeviceControlWidget::updateControlsState()
    {
        // Keep button state in sync
        if (m_exposureLineEdit)
        {
            m_exposureLineEdit->setEnabled(m_cameraInitialized);
        }

        if (m_previewToggleButton)
        {
            m_previewToggleButton->setEnabled(m_cameraInitialized);
            m_previewToggleButton->setText(m_previewRunning
                                               ? QStringLiteral("Stop Preview")
                                               : QStringLiteral("Start Preview"));
        }
        if (m_drawROIButton)
        {
            m_drawROIButton->setEnabled(m_cameraInitialized && !isAllTarget(m_currentTarget));
        }
        if (m_clearROIButton)
        {
            m_clearROIButton->setEnabled(m_cameraInitialized);
        }

        updateStageControlsEnabled();
    }

    void DeviceControlWidget::updateCameraParametersFromHardware()
    {
        if (!m_scopeonecore->hasCore() || !m_cameraInitialized)
        {
            return;
        }

        double exposure = 0.0;
        if (m_scopeonecore->readExposure(m_currentTarget, exposure))
        {
            m_exposureLineEdit->blockSignals(true);
            m_exposureLineEdit->setText(QString::number(exposure, 'f', 1));
            m_exposureLineEdit->blockSignals(false);
        }
    }

    void DeviceControlWidget::setPreviewRunning(bool running)
    {
        m_previewRunning = running;
        updateControlsState();
    }

    bool DeviceControlWidget::isAllTarget(const QString& target) const
    {
        return target.compare("All", Qt::CaseInsensitive) == 0;
    }

    void DeviceControlWidget::setControlTargets(const QStringList& cameraIds)
    {
        if (!m_cameraSelectCombo)
        {
            return;
        }

        QString current = m_cameraSelectCombo->currentText();

        {
            QSignalBlocker blocker(m_cameraSelectCombo);
            m_cameraSelectCombo->clear();
            m_cameraSelectCombo->addItem("All");
            for (const QString& id : cameraIds)
            {
                m_cameraSelectCombo->addItem(id);
            }

            int idx = m_cameraSelectCombo->findText(current);
            const bool currentIsAll = isAllTarget(current);
            if (!cameraIds.isEmpty())
            {
                if (currentIsAll)
                {
                    m_cameraSelectCombo->setCurrentIndex(1);
                }
                else if (idx >= 0)
                {
                    m_cameraSelectCombo->setCurrentIndex(idx);
                }
                else
                {
                    m_cameraSelectCombo->setCurrentIndex(1);
                }
            }
            else
            {
                m_cameraSelectCombo->setCurrentIndex(0);
            }
        }

        onControlTargetSelectionChanged(m_cameraSelectCombo->currentText());
    }

    bool DeviceControlWidget::acceptsCameraStream(const QString& cameraId) const
    {
        if (cameraId.isEmpty())
        {
            return false;
        }
        if (isAllTarget(m_currentTarget))
        {
            return true;
        }

        return m_currentTarget == cameraId;
    }

    void DeviceControlWidget::onPreviewToggleClicked()
    {
        // Use one button for preview
        if (m_previewRunning)
        {
            emit stopPreviewRequested();
        }
        else
        {
            emit startPreviewRequested();
        }
    }

    void DeviceControlWidget::setControlTargetEnabled(bool enabled)
    {
        if (m_cameraSelectCombo)
        {
            m_cameraSelectCombo->setEnabled(enabled);
        }
    }

    void DeviceControlWidget::onControlTargetSelectionChanged(const QString& target)
    {
        const QString normalizedTarget = target.trimmed();
        if (normalizedTarget.isEmpty())
        {
            return;
        }
        m_currentTarget = normalizedTarget;
        updateControlsState();
        updateCameraParametersFromHardware();
        emit controlTargetChanged(normalizedTarget);
    }

    void DeviceControlWidget::onDrawROIClicked()
    {
        if (isAllTarget(m_currentTarget))
        {
            return;
        }

        emit requestDrawROI(m_currentTarget);
    }

    void DeviceControlWidget::onClearROIClicked()
    {
        emit requestClearROI(m_currentTarget);
    }
} // namespace scopeone::ui
