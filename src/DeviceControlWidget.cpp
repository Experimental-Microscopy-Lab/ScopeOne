#include "DeviceControlWidget.h"
#include "scopeone/ScopeOneCore.h"
#include "PreviewWidget.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace scopeone::ui
{
    namespace
    {
        // Converts a preview layout mode to combo index
        int layerLayoutComboIndex(PreviewWidget::LayerLayoutMode mode)
        {
            return mode == PreviewWidget::LayerLayoutMode::Overlay ? 1 : 0;
        }

        // Converts a combo index to preview layout mode
        PreviewWidget::LayerLayoutMode layerLayoutModeFromComboIndex(int index)
        {
            return index == 1
                       ? PreviewWidget::LayerLayoutMode::Overlay
                       : PreviewWidget::LayerLayoutMode::SideBySide;
        }

        // Formats exposure with compact decimal precision
        QString formatExposureMs(double exposureMs)
        {
            QString text = QString::number(exposureMs, 'f', 4);
            while (text.endsWith(QLatin1Char('0')))
            {
                text.chop(1);
            }
            if (text.endsWith(QLatin1Char('.')))
            {
                text.chop(1);
            }
            return text;
        }
    } // namespace

    // Creates the device control widget and initializes controls
    DeviceControlWidget::DeviceControlWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
          , m_cameraInitialized(false)
          , m_previewRunning(false)
          , m_currentTarget("All")
    {
        if (!core)
        {
            qFatal("DeviceControlWidget requires ScopeOneCore");
        }

        setWindowTitle("Control");

        setupUI();
        updateControlsState();
        refreshStageDevices();
        m_currentTarget = m_cameraSelectCombo->currentText();
    }

    // Builds the device control layout
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

    // Connects the preview widget to control panel state
    void DeviceControlWidget::setPreviewWidget(PreviewWidget* preview)
    {
        if (!preview)
        {
            qFatal("DeviceControlWidget requires PreviewWidget");
        }
        m_previewWidget = preview;

        {
            QSignalBlocker blocker(m_layerColormapComboBox);
            m_layerColormapComboBox->clear();
            m_layerColormapComboBox->addItems(m_previewWidget->supportedLayerColormaps());
        }
        {
            QSignalBlocker blocker(m_layerBlendingComboBox);
            m_layerBlendingComboBox->clear();
            m_layerBlendingComboBox->addItems(m_previewWidget->supportedLayerBlendingModes());
        }

        connect(m_previewWidget, &PreviewWidget::availableCameraIdsChanged,
                this, &DeviceControlWidget::onPreviewAvailableCameraIdsChanged);
        connect(m_previewWidget, &PreviewWidget::availableLayerKeysChanged,
                this, &DeviceControlWidget::onPreviewAvailableLayerKeysChanged);
        connect(m_previewWidget, &PreviewWidget::selectedLayerKeysChanged,
                this, [this](const QStringList& layerKeys)
                {
                    applyPreviewSelection(layerKeys, false);
                });
        connect(m_previewWidget, &PreviewWidget::layerLayoutModeChanged,
                this, [this](PreviewWidget::LayerLayoutMode mode)
                {
                    syncPreviewLayerLayoutCombo(layerLayoutComboIndex(mode));
                });
        connect(m_previewWidget, &PreviewWidget::layerInfoTextChanged,
                this, &DeviceControlWidget::onPreviewLayerInfoTextChanged);
        connect(m_previewWidget, &PreviewWidget::zoomLevelChanged, this,
                [this](int value)
                {
                    const QSignalBlocker blocker(m_zoomSpinBox);
                    m_zoomSpinBox->setValue(value);
                });
        connect(m_previewWidget, &PreviewWidget::fitToWindowChanged, this,
                [this](bool enabled)
                {
                    const QSignalBlocker blocker(m_fitToWindowCheckBox);
                    m_fitToWindowCheckBox->setChecked(enabled);
                    updatePreviewZoomControls();
                });

        onPreviewAvailableCameraIdsChanged(m_previewWidget->availableCameraIds());
        onPreviewAvailableLayerKeysChanged(m_previewWidget->availableLayerKeys());
        applyPreviewSelection(m_previewWidget->selectedLayerKeys(), false);
        syncPreviewLayerLayoutCombo(layerLayoutComboIndex(m_previewWidget->layerLayoutMode()));
        onPreviewLayerInfoTextChanged(m_previewWidget->layerInfoSummaryText());

        QSignalBlocker zoomBlocker(m_zoomSpinBox);
        m_zoomSpinBox->setValue(m_previewWidget->zoomPercent());
        QSignalBlocker fitBlocker(m_fitToWindowCheckBox);
        m_fitToWindowCheckBox->setChecked(m_previewWidget->isFitToWindow());
        updatePreviewZoomControls();
    }

    // Builds preview zoom layer and alignment controls
    QWidget* DeviceControlWidget::createPreviewControlsGroup()
    {
        m_previewControlsGroup = new QGroupBox("Preview Controls", this);
        QGridLayout* controlLayout = new QGridLayout(m_previewControlsGroup);
        controlLayout->setHorizontalSpacing(6);
        controlLayout->setVerticalSpacing(4);
        controlLayout->setContentsMargins(6, 6, 6, 6);

        m_zoomLabel = new QLabel("View Zoom:", this);
        m_zoomSpinBox = new QSpinBox(this);
        m_zoomSpinBox->setRange(10, 500);
        m_zoomSpinBox->setValue(100);
        m_zoomSpinBox->setSuffix("%");
        m_zoomSpinBox->setFixedWidth(58);
        m_zoomSpinBox->setKeyboardTracking(false);

        m_fitToWindowCheckBox = new QCheckBox("Fit to Window", this);
        m_fitToWindowCheckBox->setChecked(true);

        m_layerLayoutCombo = new QComboBox(this);
        m_layerLayoutCombo->addItem("Side-by-side");
        m_layerLayoutCombo->addItem("Overlay");

        m_layerTable = new QTableWidget(this);
        m_layerTable->setColumnCount(3);
        m_layerTable->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("Show"),
            QStringLiteral("Layer"),
            QStringLiteral("Info"),
        });
        m_layerTable->verticalHeader()->setVisible(false);
        m_layerTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_layerTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_layerTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_layerTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_layerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_layerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_layerTable->setShowGrid(false);
        m_layerTable->setMinimumHeight(94);
        m_layerTable->setMaximumHeight(150);

        m_layerSettingsGroup = new QGroupBox("Layer Settings", this);
        QGridLayout* layerSettingsLayout = new QGridLayout(m_layerSettingsGroup);
        layerSettingsLayout->setContentsMargins(6, 6, 6, 6);
        layerSettingsLayout->setHorizontalSpacing(6);
        layerSettingsLayout->setVerticalSpacing(4);

        m_selectedLayerLabel = new QLabel(QStringLiteral("No layer selected"), m_layerSettingsGroup);
        m_selectedLayerLabel->setWordWrap(true);

        m_layerMoveUpButton = new QPushButton(QStringLiteral("Up"), m_layerSettingsGroup);
        m_layerMoveDownButton = new QPushButton(QStringLiteral("Down"), m_layerSettingsGroup);
        m_layerRemoveButton = new QPushButton(QStringLiteral("Remove"), m_layerSettingsGroup);
        m_layerRemoveButton->setMaximumWidth(68);

        m_layerOpacitySpinBox = new QSpinBox(m_layerSettingsGroup);
        m_layerOpacitySpinBox->setRange(0, 100);
        m_layerOpacitySpinBox->setSuffix(QStringLiteral("%"));
        m_layerOpacitySpinBox->setFixedWidth(58);
        m_layerOpacitySpinBox->setKeyboardTracking(false);

        m_layerGammaSpinBox = new QDoubleSpinBox(m_layerSettingsGroup);
        m_layerGammaSpinBox->setRange(0.2, 2.0);
        m_layerGammaSpinBox->setDecimals(2);
        m_layerGammaSpinBox->setSingleStep(0.02);
        m_layerGammaSpinBox->setFixedWidth(64);
        m_layerGammaSpinBox->setKeyboardTracking(false);

        m_layerColormapComboBox = new QComboBox(m_layerSettingsGroup);
        m_layerBlendingComboBox = new QComboBox(m_layerSettingsGroup);

        m_layerFrameLabel = new QLabel(QStringLiteral("Frame:"), m_layerSettingsGroup);
        m_layerFrameSlider = new QSlider(Qt::Horizontal, m_layerSettingsGroup);
        m_layerFrameSlider->setRange(0, 0);
        m_layerFrameValueLabel = new QLabel(QStringLiteral("1 / 1"), m_layerSettingsGroup);
        m_layerFrameValueLabel->setMinimumWidth(46);
        m_layerFrameValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        layerSettingsLayout->addWidget(m_selectedLayerLabel, 0, 0, 1, 6);
        layerSettingsLayout->addWidget(m_layerFrameLabel, 1, 0);
        layerSettingsLayout->addWidget(m_layerFrameSlider, 1, 1, 1, 4);
        layerSettingsLayout->addWidget(m_layerFrameValueLabel, 1, 5);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Order:"), m_layerSettingsGroup), 2, 0);
        layerSettingsLayout->addWidget(m_layerMoveUpButton, 2, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_layerMoveDownButton, 2, 2, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_layerRemoveButton, 2, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Opacity:"), m_layerSettingsGroup), 3, 0);
        layerSettingsLayout->addWidget(m_layerOpacitySpinBox, 3, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Gamma:"), m_layerSettingsGroup), 3, 2);
        layerSettingsLayout->addWidget(m_layerGammaSpinBox, 3, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Color:"), m_layerSettingsGroup), 4, 0);
        layerSettingsLayout->addWidget(m_layerColormapComboBox, 4, 1);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Blend:"), m_layerSettingsGroup), 4, 2);
        layerSettingsLayout->addWidget(m_layerBlendingComboBox, 4, 3, 1, 3);

        m_alignXLabel = new QLabel("X offset:", m_layerSettingsGroup);
        m_alignXSpinBox = new QSpinBox(m_layerSettingsGroup);
        m_alignXSpinBox->setRange(-1000, 1000);
        m_alignXSpinBox->setValue(0);
        m_alignXSpinBox->setFixedWidth(64);
        m_alignXSpinBox->setKeyboardTracking(false);

        m_alignYLabel = new QLabel("Y offset:", m_layerSettingsGroup);
        m_alignYSpinBox = new QSpinBox(m_layerSettingsGroup);
        m_alignYSpinBox->setRange(-1000, 1000);
        m_alignYSpinBox->setValue(0);
        m_alignYSpinBox->setFixedWidth(64);
        m_alignYSpinBox->setKeyboardTracking(false);

        m_alignZoomLabel = new QLabel("Scale:", m_layerSettingsGroup);
        m_alignZoomSpinBox = new QSpinBox(m_layerSettingsGroup);
        m_alignZoomSpinBox->setRange(10, 500);
        m_alignZoomSpinBox->setValue(100);
        m_alignZoomSpinBox->setFixedWidth(58);
        m_alignZoomSpinBox->setToolTip("Source camera display scale percent");
        m_alignZoomSpinBox->setKeyboardTracking(false);

        m_alignFlipXCheckBox = new QCheckBox("Flip X", m_layerSettingsGroup);
        m_alignFlipYCheckBox = new QCheckBox("Flip Y", m_layerSettingsGroup);
        m_alignResetButton = new QPushButton("Reset", m_layerSettingsGroup);
        m_alignResetButton->setMaximumWidth(50);
        m_alignResetButton->setToolTip("Reset offset and flip");

        // Display transforms are edited from the selected layer but stored per source camera for now
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Transform:"), m_layerSettingsGroup), 5, 0, 1, 6);
        layerSettingsLayout->addWidget(m_alignXLabel, 6, 0);
        layerSettingsLayout->addWidget(m_alignXSpinBox, 6, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_alignYLabel, 6, 2);
        layerSettingsLayout->addWidget(m_alignYSpinBox, 6, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_alignZoomLabel, 7, 0);
        layerSettingsLayout->addWidget(m_alignZoomSpinBox, 7, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_alignFlipXCheckBox, 7, 2, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_alignFlipYCheckBox, 7, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_alignResetButton, 7, 4, Qt::AlignLeft);
        layerSettingsLayout->setColumnStretch(5, 1);

        m_zoomLabel->setMinimumWidth(60);
        m_alignXLabel->setMinimumWidth(20);
        m_alignYLabel->setMinimumWidth(20);
        m_alignZoomLabel->setMinimumWidth(60);

        QLabel* layoutLabel = new QLabel("Layout:", this);
        layoutLabel->setMinimumWidth(60);

        controlLayout->addWidget(m_zoomLabel, 0, 0);
        controlLayout->addWidget(m_zoomSpinBox, 0, 1, Qt::AlignLeft);
        controlLayout->addWidget(m_fitToWindowCheckBox, 0, 2, 1, 2);
        controlLayout->addWidget(layoutLabel, 0, 4);
        controlLayout->addWidget(m_layerLayoutCombo, 0, 5);

        controlLayout->addWidget(m_layerTable, 1, 0, 1, 6);
        controlLayout->addWidget(m_layerSettingsGroup, 2, 0, 1, 6);

        controlLayout->setColumnStretch(1, 1);
        controlLayout->setColumnStretch(3, 1);
        controlLayout->setColumnStretch(5, 1);

        connect(m_zoomSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewZoomSpinBoxChanged);
        connect(m_fitToWindowCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onPreviewFitToWindowToggled);
        connect(m_layerLayoutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerLayoutComboChanged);
        connect(m_layerTable, &QTableWidget::currentCellChanged,
                this, &DeviceControlWidget::onPreviewLayerSelectionChanged);
        connect(m_layerMoveUpButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerMoveUpClicked);
        connect(m_layerMoveDownButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerMoveDownClicked);
        connect(m_layerRemoveButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerRemoveClicked);
        connect(m_layerOpacitySpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerOpacityChanged);
        connect(m_layerGammaSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerGammaChanged);
        connect(m_layerColormapComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerColormapChanged);
        connect(m_layerBlendingComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerBlendingChanged);
        connect(m_layerFrameSlider, &QSlider::valueChanged,
                this, &DeviceControlWidget::onPreviewLayerFrameSliderChanged);

        connect(m_alignXSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int x)
                {
                    const QString sourceId = selectedLayerSourceId();
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    m_previewWidget->setSourceOffset(sourceId, x, m_alignYSpinBox->value());
                });
        connect(m_alignYSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int y)
                {
                    const QString sourceId = selectedLayerSourceId();
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    m_previewWidget->setSourceOffset(sourceId, m_alignXSpinBox->value(), y);
                });
        connect(m_alignZoomSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int percent)
                {
                    const QString sourceId = selectedLayerSourceId();
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    m_previewWidget->setSourceZoomPercent(sourceId, percent);
                });
        connect(m_alignFlipXCheckBox, &QCheckBox::toggled,
                this, [this](bool enabled)
                {
                    const QString sourceId = selectedLayerSourceId();
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    m_previewWidget->setSourceFlip(sourceId, enabled, m_alignFlipYCheckBox->isChecked());
                });
        connect(m_alignFlipYCheckBox, &QCheckBox::toggled,
                this, [this](bool enabled)
                {
                    const QString sourceId = selectedLayerSourceId();
                    if (sourceId.isEmpty())
                    {
                        return;
                    }
                    m_previewWidget->setSourceFlip(sourceId, m_alignFlipXCheckBox->isChecked(), enabled);
                });
        connect(m_alignResetButton, &QPushButton::clicked,
                this, [this]()
                {
                    resetSelectedLayerTransform();
                });

        updatePreviewZoomControls();
        refreshPreviewLayerSettings();
        return m_previewControlsGroup;
    }

    // Updates zoom controls from fit to window state
    void DeviceControlWidget::updatePreviewZoomControls()
    {
        m_zoomSpinBox->setEnabled(!m_fitToWindowCheckBox->isChecked());
    }

    // Rebuilds the preview layer table
    void DeviceControlWidget::rebuildPreviewLayerTable(const QStringList& layerKeys)
    {
        const QString previousLayerKey = m_selectedLayerKey;
        QSignalBlocker tableBlocker(m_layerTable);
        m_layerRows.clear();
        m_layerTable->setRowCount(layerKeys.size());
        for (const QString& layerKey : m_layerFrameCounts.keys())
        {
            if (!layerKeys.contains(layerKey))
            {
                m_layerFrameCounts.remove(layerKey);
                m_layerFrameIndices.remove(layerKey);
            }
        }

        int row = 0;
        const auto addLayerRow = [&](const QString& layerKey)
        {
            QCheckBox* visibleCheckBox = new QCheckBox(m_layerTable);
            visibleCheckBox->setProperty("layerKey", layerKey);
            connect(visibleCheckBox, &QCheckBox::toggled,
                    this, &DeviceControlWidget::onPreviewLayerVisibleToggled);
            m_layerTable->setCellWidget(row, 0, visibleCheckBox);

            QTableWidgetItem* nameItem = new QTableWidgetItem(m_previewWidget->layerName(layerKey));
            nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            nameItem->setData(Qt::UserRole, layerKey);
            m_layerTable->setItem(row, 1, nameItem);

            QTableWidgetItem* infoItem = new QTableWidgetItem(m_previewWidget->layerInfoText(layerKey));
            infoItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            infoItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            infoItem->setForeground(QColor(102, 102, 102));
            m_layerTable->setItem(row, 2, infoItem);

            m_layerRows.insert(layerKey, visibleCheckBox);
            m_layerTable->setRowHeight(row, 26);
            ++row;
        };

        for (const QString& layerKey : layerKeys)
        {
            addLayerRow(layerKey);
        }

        int selectedRow = layerKeys.indexOf(previousLayerKey);
        if (selectedRow < 0 && !layerKeys.isEmpty())
        {
            selectedRow = 0;
        }

        if (selectedRow >= 0)
        {
            m_layerTable->setCurrentCell(selectedRow, 1);
            m_selectedLayerKey = layerKeys.at(selectedRow);
        }
        else
        {
            m_selectedLayerKey.clear();
        }

        refreshPreviewLayerSettings();
        if (m_selectedLayerKey != previousLayerKey)
        {
            emit currentLayerChanged(m_selectedLayerKey);
            syncControlTargetToSelectedRawLayer();
        }
    }

    // Refreshes per layer size and frame rate text in the layer table
    void DeviceControlWidget::refreshPreviewLayerInfoText()
    {
        for (int row = 0; row < m_layerTable->rowCount(); ++row)
        {
            QTableWidgetItem* nameItem = m_layerTable->item(row, 1);
            QTableWidgetItem* infoItem = m_layerTable->item(row, 2);
            infoItem->setText(m_previewWidget->layerInfoText(nameItem->data(Qt::UserRole).toString()));
        }
    }

    // Applies a layer selection to layer rows and preview
    void DeviceControlWidget::applyPreviewSelection(const QStringList& layerKeys, bool notifyPreview)
    {
        QSet<QString> selectedLayerKeySet;
        for (const QString& layerKey : layerKeys)
        {
            selectedLayerKeySet.insert(layerKey);
        }

        for (auto it = m_layerRows.begin(); it != m_layerRows.end(); ++it)
        {
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(selectedLayerKeySet.contains(it.key()));
        }

        if (notifyPreview)
        {
            m_previewWidget->setSelectedLayerKeys(layerKeys);
        }

        const QString previousLayerKey = m_selectedLayerKey;
        if (!layerKeys.isEmpty() && !selectedLayerKeySet.contains(m_selectedLayerKey))
        {
            const QString nextLayerKey = layerKeys.first();
            for (int row = 0; row < m_layerTable->rowCount(); ++row)
            {
                QTableWidgetItem* item = m_layerTable->item(row, 1);
                if (item->data(Qt::UserRole).toString() == nextLayerKey)
                {
                    QSignalBlocker tableBlocker(m_layerTable);
                    m_layerTable->setCurrentCell(row, 1);
                    m_selectedLayerKey = nextLayerKey;
                    break;
                }
            }
        }

        refreshPreviewLayerSettings();
        if (m_selectedLayerKey != previousLayerKey)
        {
            emit currentLayerChanged(m_selectedLayerKey);
        }
    }

    // Updates the settings editor for the selected preview layer
    void DeviceControlWidget::refreshPreviewLayerSettings()
    {
        const bool hasLayer = !m_selectedLayerKey.isEmpty()
                              && m_layerRows.contains(m_selectedLayerKey);
        m_layerSettingsGroup->setEnabled(hasLayer);
        if (!hasLayer)
        {
            m_selectedLayerLabel->setText(QStringLiteral("No layer selected"));
            refreshLayerFrameControl();
            return;
        }

        m_selectedLayerLabel->setText(m_previewWidget->layerName(m_selectedLayerKey));
        refreshLayerFrameControl();

        {
            QSignalBlocker blocker(m_layerOpacitySpinBox);
            m_layerOpacitySpinBox->setValue(m_previewWidget->layerOpacityPercent(m_selectedLayerKey));
        }
        {
            QSignalBlocker blocker(m_layerGammaSpinBox);
            m_layerGammaSpinBox->setValue(m_previewWidget->layerGamma(m_selectedLayerKey));
        }
        {
            QSignalBlocker blocker(m_layerColormapComboBox);
            const int index = m_layerColormapComboBox->findText(m_previewWidget->layerColormap(m_selectedLayerKey));
            m_layerColormapComboBox->setCurrentIndex(index);
        }
        {
            QSignalBlocker blocker(m_layerBlendingComboBox);
            const int index = m_layerBlendingComboBox->findText(m_previewWidget->layerBlending(m_selectedLayerKey));
            m_layerBlendingComboBox->setCurrentIndex(index);
        }

        const int row = m_layerTable->currentRow();
        m_layerMoveUpButton->setEnabled(row > 0);
        m_layerMoveDownButton->setEnabled(row < m_layerTable->rowCount() - 1);
        m_layerRemoveButton->setEnabled(scopeone::core::ScopeOneCore::isStaticLayerKey(m_selectedLayerKey));
        m_layerOpacitySpinBox->setEnabled(m_layerBlendingComboBox->currentText() != QStringLiteral("Opaque"));

        int offsetX = 0;
        int offsetY = 0;
        int zoomPercent = 100;
        bool flipX = false;
        bool flipY = false;
        const QString sourceId = selectedLayerSourceId();
        if (!sourceId.isEmpty())
        {
            m_previewWidget->sourceDisplayTransform(sourceId, offsetX, offsetY, zoomPercent, flipX, flipY);
        }
        {
            QSignalBlocker blocker(m_alignXSpinBox);
            m_alignXSpinBox->setValue(offsetX);
        }
        {
            QSignalBlocker blocker(m_alignYSpinBox);
            m_alignYSpinBox->setValue(offsetY);
        }
        {
            QSignalBlocker blocker(m_alignZoomSpinBox);
            m_alignZoomSpinBox->setValue(zoomPercent);
        }
        {
            QSignalBlocker blocker(m_alignFlipXCheckBox);
            m_alignFlipXCheckBox->setChecked(flipX);
        }
        {
            QSignalBlocker blocker(m_alignFlipYCheckBox);
            m_alignFlipYCheckBox->setChecked(flipY);
        }
    }

    // Updates the frame slider for stack backed gallery layers
    void DeviceControlWidget::refreshLayerFrameControl()
    {
        const int frameCount = m_layerFrameCounts.value(m_selectedLayerKey, 1);
        const int frameIndex = qBound(0, m_layerFrameIndices.value(m_selectedLayerKey, 0), qMax(0, frameCount - 1));
        const bool visible = frameCount > 1;

        m_layerFrameLabel->setVisible(visible);
        m_layerFrameSlider->setVisible(visible);
        m_layerFrameValueLabel->setVisible(visible);
        m_layerFrameSlider->setEnabled(visible);

        QSignalBlocker blocker(m_layerFrameSlider);
        m_layerFrameSlider->setRange(0, qMax(0, frameCount - 1));
        m_layerFrameSlider->setValue(frameIndex);
        m_layerFrameValueLabel->setText(QStringLiteral("%1 / %2").arg(frameIndex + 1).arg(qMax(1, frameCount)));
    }

    QString DeviceControlWidget::selectedLayerSourceId() const
    {
        return scopeone::core::ScopeOneCore::sourceIdFromLayerKey(m_selectedLayerKey);
    }

    // Refreshes selected layer transform values when live cameras change
    void DeviceControlWidget::onPreviewAvailableCameraIdsChanged(const QStringList&)
    {
        refreshPreviewLayerSettings();
    }

    void DeviceControlWidget::onPreviewAvailableLayerKeysChanged(const QStringList& layerKeys)
    {
        rebuildPreviewLayerTable(layerKeys);
        applyPreviewSelection(m_previewWidget->selectedLayerKeys(), false);
        updateControlsState();
    }

    void DeviceControlWidget::syncPreviewLayerLayoutCombo(int index)
    {
        QSignalBlocker blocker(m_layerLayoutCombo);
        m_layerLayoutCombo->setCurrentIndex(index);
    }

    void DeviceControlWidget::onPreviewLayerInfoTextChanged(const QString&)
    {
        refreshPreviewLayerInfoText();
    }

    void DeviceControlWidget::onPreviewZoomSpinBoxChanged(int value)
    {
        m_previewWidget->setZoomPercent(value);
    }

    void DeviceControlWidget::onPreviewFitToWindowToggled(bool enabled)
    {
        m_previewWidget->setFitToWindow(enabled);
        updatePreviewZoomControls();
    }

    void DeviceControlWidget::onPreviewLayerLayoutComboChanged(int index)
    {
        m_previewWidget->setLayerLayoutMode(layerLayoutModeFromComboIndex(index));
    }

    void DeviceControlWidget::onPreviewLayerVisibleToggled(bool)
    {
        auto* checkBox = qobject_cast<QCheckBox*>(sender());
        const QString layerKey = checkBox->property("layerKey").toString();
        for (int row = 0; row < m_layerTable->rowCount(); ++row)
        {
            QTableWidgetItem* item = m_layerTable->item(row, 1);
            if (item->data(Qt::UserRole).toString() == layerKey)
            {
                m_layerTable->setCurrentCell(row, 1);
                break;
            }
        }
        m_previewWidget->setLayerVisible(layerKey, checkBox->isChecked());
    }

    void DeviceControlWidget::onPreviewLayerOpacityChanged(int value)
    {
        m_previewWidget->setLayerOpacityPercent(m_selectedLayerKey, value);
    }

    void DeviceControlWidget::onPreviewLayerGammaChanged(double value)
    {
        m_previewWidget->setLayerGamma(m_selectedLayerKey, value);
    }

    void DeviceControlWidget::onPreviewLayerColormapChanged(int)
    {
        m_previewWidget->setLayerColormap(m_selectedLayerKey, m_layerColormapComboBox->currentText());
    }

    void DeviceControlWidget::onPreviewLayerBlendingChanged(int)
    {
        m_previewWidget->setLayerBlending(m_selectedLayerKey, m_layerBlendingComboBox->currentText());
        m_layerOpacitySpinBox->setEnabled(m_layerBlendingComboBox->currentText() != QStringLiteral("Opaque"));
    }

    void DeviceControlWidget::onPreviewLayerFrameSliderChanged(int value)
    {
        const int frameCount = m_layerFrameCounts.value(m_selectedLayerKey, 1);
        if (m_selectedLayerKey.isEmpty() || frameCount <= 1)
        {
            return;
        }

        const int frameIndex = qBound(0, value, frameCount - 1);
        if (m_layerFrameIndices.value(m_selectedLayerKey, 0) == frameIndex)
        {
            return;
        }

        m_layerFrameIndices.insert(m_selectedLayerKey, frameIndex);
        m_layerFrameValueLabel->setText(QStringLiteral("%1 / %2").arg(frameIndex + 1).arg(frameCount));
        emit previewLayerFrameRequested(m_selectedLayerKey, frameIndex);
    }

    void DeviceControlWidget::onPreviewLayerSelectionChanged(int currentRow, int, int, int)
    {
        QTableWidgetItem* item = m_layerTable->item(currentRow, 1);
        const QString previousLayerKey = m_selectedLayerKey;
        m_selectedLayerKey = item ? item->data(Qt::UserRole).toString() : QString();
        refreshPreviewLayerSettings();
        updateControlsState();
        if (m_selectedLayerKey != previousLayerKey)
        {
            emit currentLayerChanged(m_selectedLayerKey);
        }
    }

    void DeviceControlWidget::onPreviewLayerMoveUpClicked()
    {
        m_previewWidget->moveLayer(m_selectedLayerKey, -1);
    }

    void DeviceControlWidget::onPreviewLayerMoveDownClicked()
    {
        m_previewWidget->moveLayer(m_selectedLayerKey, 1);
    }

    void DeviceControlWidget::onPreviewLayerRemoveClicked()
    {
        m_scopeonecore->removeStaticFrame(
            scopeone::core::ScopeOneCore::sourceIdFromLayerKey(m_selectedLayerKey));
    }

    // Use the selected raw camera layer as the hardware control target
    void DeviceControlWidget::syncControlTargetToSelectedRawLayer()
    {
        if (!scopeone::core::ScopeOneCore::isRawLayerKey(m_selectedLayerKey))
        {
            return;
        }

        const QString sourceId = selectedLayerSourceId();
        const int index = m_cameraSelectCombo->findText(sourceId);
        if (index < 0 || m_cameraSelectCombo->currentIndex() == index)
        {
            return;
        }

        m_cameraSelectCombo->setCurrentIndex(index);
    }

    void DeviceControlWidget::resetSelectedLayerTransform()
    {
        QSignalBlocker bx(*m_alignXSpinBox);
        QSignalBlocker by(*m_alignYSpinBox);
        m_alignXSpinBox->setValue(0);
        m_alignYSpinBox->setValue(0);

        QSignalBlocker bz(*m_alignZoomSpinBox);
        m_alignZoomSpinBox->setValue(100);

        QSignalBlocker bfx(*m_alignFlipXCheckBox);
        QSignalBlocker bfy(*m_alignFlipYCheckBox);
        m_alignFlipXCheckBox->setChecked(false);
        m_alignFlipYCheckBox->setChecked(false);

        const QString sourceId = selectedLayerSourceId();
        if (!sourceId.isEmpty())
        {
            m_previewWidget->setSourceOffset(sourceId, 0, 0);
            m_previewWidget->setSourceFlip(sourceId, false, false);
            m_previewWidget->setSourceZoomPercent(sourceId, 100);
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
        auto* exposureValidator = new QDoubleValidator(0.1, 10000.0, 16, m_exposureLineEdit);
        exposureValidator->setLocale(QLocale::c());
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

        QHBoxLayout* roiLayout = new QHBoxLayout();
        m_drawROIButton = new QPushButton("Draw ROI", group);
        m_halfROIButton = new QPushButton("Half ROI", group);
        m_clearROIButton = new QPushButton("Restore ROI", group);
        roiLayout->addWidget(m_drawROIButton);
        roiLayout->addWidget(m_halfROIButton);
        roiLayout->addWidget(m_clearROIButton);
        layout->addLayout(roiLayout, ++row, 0, 1, 2);
        connect(m_drawROIButton, &QPushButton::clicked, this, &DeviceControlWidget::onDrawROIClicked);
        connect(m_halfROIButton, &QPushButton::clicked, this, &DeviceControlWidget::onHalfROIClicked);
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
        m_xyLeftButton = new QPushButton(QStringLiteral("<"));
        m_xyRightButton = new QPushButton(QStringLiteral(">"));
        m_xyBigUpButton = new QPushButton(QStringLiteral("↑↑"));
        m_xyBigDownButton = new QPushButton(QStringLiteral("↓↓"));
        m_xyBigLeftButton = new QPushButton(QStringLiteral("<<"));
        m_xyBigRightButton = new QPushButton(QStringLiteral(">>"));
        auto setArrowSize = [](QPushButton* button)
        {
            button->setFixedSize(28, 28);
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
        xyCenterWidget->setFixedWidth(60);
        QVBoxLayout* xyCenterLayout = new QVBoxLayout(xyCenterWidget);
        xyCenterLayout->setContentsMargins(0, 0, 0, 0);
        xyCenterLayout->setSpacing(2);
        m_xPosLabel = new QLabel("X: N/A");
        m_yPosLabel = new QLabel("Y: N/A");
        m_xPosLabel->setFixedWidth(60);
        m_yPosLabel->setFixedWidth(60);
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
        xyPad->setAlignment(m_xyBigUpButton, Qt::AlignHCenter);
        xyPad->setAlignment(m_xyUpButton, Qt::AlignHCenter);
        xyPad->setAlignment(xyCenterWidget, Qt::AlignHCenter);
        xyPad->setAlignment(m_xyDownButton, Qt::AlignHCenter);
        xyPad->setAlignment(m_xyBigDownButton, Qt::AlignHCenter);

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
        m_zBigUpButton = new QPushButton(QStringLiteral("↑↑"));
        m_zBigDownButton = new QPushButton(QStringLiteral("↓↓"));
        setArrowSize(m_zUpButton);
        setArrowSize(m_zDownButton);
        setArrowSize(m_zBigUpButton);
        setArrowSize(m_zBigDownButton);
        zButtonsLayout->addWidget(m_zBigUpButton);
        zButtonsLayout->addWidget(m_zUpButton);
        m_zPosLabel = new QLabel("Z: N/A");
        m_zPosLabel->setFixedWidth(60);
        m_zPosLabel->setAlignment(Qt::AlignCenter);
        zButtonsLayout->addWidget(m_zPosLabel);
        zButtonsLayout->addWidget(m_zDownButton);
        zButtonsLayout->addWidget(m_zBigDownButton);
        zButtonsLayout->setAlignment(m_zBigUpButton, Qt::AlignHCenter);
        zButtonsLayout->setAlignment(m_zUpButton, Qt::AlignHCenter);
        zButtonsLayout->setAlignment(m_zPosLabel, Qt::AlignHCenter);
        zButtonsLayout->setAlignment(m_zDownButton, Qt::AlignHCenter);
        zButtonsLayout->setAlignment(m_zBigDownButton, Qt::AlignHCenter);
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
        const auto moveXYWithStep = [this](const QLineEdit& lineEdit, double dxScale, double dyScale)
        {
            const double stepValue = lineEdit.text().toDouble();
            if (stepValue <= 0.0) return;
            moveXYStage(dxScale * stepValue, dyScale * stepValue);
        };
        const auto moveZWithStep = [this](const QLineEdit& lineEdit, double scale)
        {
            const double stepValue = lineEdit.text().toDouble();
            if (stepValue <= 0.0) return;
            moveZStage(scale * stepValue);
        };
        connect(m_xyUpButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyStepLineEdit, 0.0, 1.0);
        });
        connect(m_xyDownButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyStepLineEdit, 0.0, -1.0);
        });
        connect(m_xyLeftButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyStepLineEdit, -1.0, 0.0);
        });
        connect(m_xyRightButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyStepLineEdit, 1.0, 0.0);
        });
        connect(m_xyBigUpButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyBigStepLineEdit, 0.0, 1.0);
        });
        connect(m_xyBigDownButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyBigStepLineEdit, 0.0, -1.0);
        });
        connect(m_xyBigLeftButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyBigStepLineEdit, -1.0, 0.0);
        });
        connect(m_xyBigRightButton, &QPushButton::clicked, this, [this, moveXYWithStep]()
        {
            moveXYWithStep(*m_xyBigStepLineEdit, 1.0, 0.0);
        });
        connect(m_zUpButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(*m_zStepLineEdit, 1.0);
        });
        connect(m_zDownButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(*m_zStepLineEdit, -1.0);
        });
        connect(m_zBigUpButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(*m_zBigStepLineEdit, 1.0);
        });
        connect(m_zBigDownButton, &QPushButton::clicked, this, [this, moveZWithStep]()
        {
            moveZWithStep(*m_zBigStepLineEdit, -1.0);
        });

        return group;
    }

    // Refreshes available XY and Z stage devices
    void DeviceControlWidget::refreshStageDevices()
    {
        QSignalBlocker blockXY(m_xyStageCombo);
        QSignalBlocker blockZ(m_zStageCombo);
        m_xyStageCombo->clear();
        m_zStageCombo->clear();

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

        const QString preferredZ = m_scopeonecore->currentFocusDevice();
        if (!preferredZ.isEmpty())
        {
            const int idx = m_zStageCombo->findText(preferredZ);
            if (idx >= 0)
            {
                m_zStageCombo->setCurrentIndex(idx);
            }
        }

        updateStageControlsEnabled();
        updateStagePositions();
    }

    // Updates enabled state for stage controls
    void DeviceControlWidget::updateStageControlsEnabled()
    {
        const bool hasXY = !selectedXYStageLabel().isEmpty();
        const bool hasZ = !selectedZStageLabel().isEmpty();

        m_xyStageCombo->setEnabled(m_xyStageCombo->count() > 0);
        m_zStageCombo->setEnabled(m_zStageCombo->count() > 0);
        m_xyStepLineEdit->setEnabled(hasXY);
        m_xyBigStepLineEdit->setEnabled(hasXY);
        m_zStepLineEdit->setEnabled(hasZ);
        m_zBigStepLineEdit->setEnabled(hasZ);
        m_xyUpButton->setEnabled(hasXY);
        m_xyDownButton->setEnabled(hasXY);
        m_xyLeftButton->setEnabled(hasXY);
        m_xyRightButton->setEnabled(hasXY);
        m_xyBigUpButton->setEnabled(hasXY);
        m_xyBigDownButton->setEnabled(hasXY);
        m_xyBigLeftButton->setEnabled(hasXY);
        m_xyBigRightButton->setEnabled(hasXY);
        m_zUpButton->setEnabled(hasZ);
        m_zDownButton->setEnabled(hasZ);
        m_zBigUpButton->setEnabled(hasZ);
        m_zBigDownButton->setEnabled(hasZ);
    }

    // Reads current stage positions into labels
    void DeviceControlWidget::updateStagePositions()
    {
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
                m_xPosLabel->setText(QString("X: %1").arg(QString::number(x, 'f', 4)));
                m_yPosLabel->setText(QString("Y: %1").arg(QString::number(y, 'f', 4)));
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
                m_zPosLabel->setText(QString("Z: %1").arg(QString::number(z, 'f', 4)));
            }
            else
            {
                m_zPosLabel->setText("Z: N/A");
            }
        }
    }

    QString DeviceControlWidget::selectedXYStageLabel() const
    {
        return m_xyStageCombo->currentText().trimmed();
    }

    QString DeviceControlWidget::selectedZStageLabel() const
    {
        return m_zStageCombo->currentText().trimmed();
    }

    // Moves the selected XY stage by a relative offset
    void DeviceControlWidget::moveXYStage(double dx, double dy)
    {
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

    // Moves the selected Z stage by a relative offset
    void DeviceControlWidget::moveZStage(double dz)
    {
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

    // Updates control state when cameras initialize
    void DeviceControlWidget::onCameraInitialized(bool initialized)
    {
        m_cameraInitialized = initialized;
        updateControlsState();

        if (initialized)
        {
            updateCameraParametersFromHardware();
        }
    }

    // Refreshes camera parameters from hardware
    void DeviceControlWidget::refreshCameraParameters()
    {
        updateCameraParametersFromHardware();
    }

    // Applies exposure from the editor
    void DeviceControlWidget::onExposureChanged()
    {
        updateExposureLimits();

        bool ok = false;
        double exposureMs = m_exposureLineEdit->text().trimmed().toDouble(&ok);
        if (!ok)
        {
            updateCameraParametersFromHardware();
            return;
        }
        exposureMs = qBound(m_minExposureMs, exposureMs, m_maxExposureMs);

        m_exposureLineEdit->setText(formatExposureMs(exposureMs));
        emit exposureValueChanged(exposureMs);
    }

    // Keeps device control buttons in sync
    void DeviceControlWidget::updateControlsState()
    {
        m_exposureLineEdit->setEnabled(m_cameraInitialized);
        m_previewToggleButton->setEnabled(m_cameraInitialized);
        m_previewToggleButton->setText(m_previewRunning
                                           ? QStringLiteral("Stop Preview")
                                           : QStringLiteral("Start Preview"));
        const bool hasRoiTarget = !roiCameraTarget().isEmpty();
        m_drawROIButton->setEnabled(m_cameraInitialized && hasRoiTarget);
        m_halfROIButton->setEnabled(m_cameraInitialized && hasRoiTarget);
        m_clearROIButton->setEnabled(m_cameraInitialized);

        updateStageControlsEnabled();
    }

    // Reads camera parameters back from hardware
    void DeviceControlWidget::updateCameraParametersFromHardware()
    {
        if (!m_cameraInitialized)
        {
            return;
        }

        updateExposureLimits();

        double exposure = 0.0;
        if (m_scopeonecore->readExposure(m_currentTarget, exposure))
        {
            QSignalBlocker blocker(m_exposureLineEdit);
            m_exposureLineEdit->setText(formatExposureMs(exposure));
        }
    }

    // Updates exposure limits for the selected target
    void DeviceControlWidget::updateExposureLimits()
    {
        m_minExposureMs = 0.1;
        m_maxExposureMs = 10000.0;
        double lower = 0.0;
        double upper = 0.0;
        if (!isAllTarget(m_currentTarget))
        {
            if (m_scopeonecore->getPropertyLimits(m_currentTarget, QStringLiteral("Exposure"), lower, upper)
                && lower <= upper)
            {
                m_minExposureMs = lower;
                m_maxExposureMs = upper;
            }
            return;
        }

        bool hasCommonLimits = false;
        double commonLower = 0.0;
        double commonUpper = 0.0;
        for (int i = 0; i < m_cameraSelectCombo->count(); ++i)
        {
            const QString cameraId = m_cameraSelectCombo->itemText(i);
            if (isAllTarget(cameraId))
            {
                continue;
            }
            if (!m_scopeonecore->getPropertyLimits(cameraId, QStringLiteral("Exposure"), lower, upper))
            {
                continue;
            }

            if (!hasCommonLimits)
            {
                commonLower = lower;
                commonUpper = upper;
                hasCommonLimits = true;
            }
            else
            {
                commonLower = std::max(commonLower, lower);
                commonUpper = std::min(commonUpper, upper);
            }
        }

        if (hasCommonLimits && commonLower <= commonUpper)
        {
            m_minExposureMs = commonLower;
            m_maxExposureMs = commonUpper;
        }
    }

    // Updates preview running state for the control button
    void DeviceControlWidget::setPreviewRunning(bool running)
    {
        m_previewRunning = running;
        updateControlsState();
    }

    // Returns the preview layer currently selected in the layer table
    QString DeviceControlWidget::currentLayerKey() const
    {
        return m_selectedLayerKey;
    }

    // Sets stack frame metadata for one preview layer
    void DeviceControlWidget::setLayerFrameControl(const QString& layerKey, int frameCount, int frameIndex)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty() || frameCount <= 1)
        {
            removeLayerFrameControl(trimmedLayerKey);
            return;
        }

        const int clampedCount = qMax(1, frameCount);
        m_layerFrameCounts.insert(trimmedLayerKey, clampedCount);
        m_layerFrameIndices.insert(trimmedLayerKey, qBound(0, frameIndex, clampedCount - 1));
        if (trimmedLayerKey == m_selectedLayerKey)
        {
            refreshLayerFrameControl();
        }
    }

    // Removes stack frame metadata for one preview layer
    void DeviceControlWidget::removeLayerFrameControl(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        m_layerFrameCounts.remove(trimmedLayerKey);
        m_layerFrameIndices.remove(trimmedLayerKey);
        if (trimmedLayerKey.isEmpty() || trimmedLayerKey == m_selectedLayerKey)
        {
            refreshLayerFrameControl();
        }
    }

    bool DeviceControlWidget::isAllTarget(const QString& target) const
    {
        return target.compare("All", Qt::CaseInsensitive) == 0;
    }

    QString DeviceControlWidget::roiCameraTarget() const
    {
        if (!isAllTarget(m_currentTarget))
        {
            return m_currentTarget;
        }

        const QString cameraId = selectedLayerSourceId();
        return m_cameraSelectCombo->findText(cameraId) >= 0 ? cameraId : QString();
    }

    // Rebuilds available camera control targets
    void DeviceControlWidget::setControlTargets(const QStringList& cameraIds)
    {
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
                if (currentIsAll && cameraIds.size() > 1)
                {
                    m_cameraSelectCombo->setCurrentIndex(0);
                }
                else if (currentIsAll)
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

    // Emits start or stop preview request from the toggle button
    void DeviceControlWidget::onPreviewToggleClicked()
    {
        if (m_previewRunning)
        {
            emit stopPreviewRequested();
        }
        else
        {
            emit startPreviewRequested();
        }
    }

    // Enables or disables the camera target selector
    void DeviceControlWidget::setControlTargetEnabled(bool enabled)
    {
        m_cameraSelectCombo->setEnabled(enabled);
    }

    // Applies a new camera control target
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

    // Starts ROI drawing for the selected camera
    void DeviceControlWidget::onDrawROIClicked()
    {
        if (isAllTarget(m_currentTarget))
        {
            emit requestDrawROI(QString());
            return;
        }

        const QString cameraId = roiCameraTarget();
        if (cameraId.isEmpty())
        {
            return;
        }

        emit requestDrawROI(cameraId);
    }

    // Requests a centered half size ROI for the selected camera
    void DeviceControlWidget::onHalfROIClicked()
    {
        const QString cameraId = roiCameraTarget();
        if (cameraId.isEmpty())
        {
            return;
        }

        emit requestHalfROI(cameraId);
    }

    // Requests ROI clearing for the selected target
    void DeviceControlWidget::onClearROIClicked()
    {
        emit requestClearROI(m_currentTarget);
    }
} // namespace scopeone::ui
