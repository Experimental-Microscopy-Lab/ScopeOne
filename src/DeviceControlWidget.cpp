#include "DeviceControlWidget.h"
#include "ImageWorkspace.h"
#include "scopeone/ImageSceneModel.h"
#include "scopeone/ScopeOneCore.h"
#include "PreviewWidget.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace scopeone::ui
{
    namespace
    {
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

    class LayerHistogramWidget : public QWidget
    {
    public:
        using LevelsCallback = std::function<void(int minLevel, int maxLevel)>;
        using AutoLevelsCallback = std::function<void()>;

        explicit LayerHistogramWidget(QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setMinimumHeight(150);
            setMouseTracking(true);
        }

        void setStats(const scopeone::core::ScopeOneCore::HistogramStats& stats)
        {
            m_stats = stats;
            update();
        }

        void setLevels(int minLevel, int maxLevel, int domainMax)
        {
            m_minLevel = minLevel;
            m_maxLevel = maxLevel;
            m_domainMax = qMax(1, domainMax);
            update();
        }

        void clear()
        {
            m_stats = {};
            update();
        }

        void setLogScale(bool enabled)
        {
            m_logScale = enabled;
            update();
        }

        void setOnLevelsChanged(LevelsCallback callback)
        {
            m_levelsCallback = std::move(callback);
        }

        void setOnAutoLevelsRequested(AutoLevelsCallback callback)
        {
            m_autoLevelsCallback = std::move(callback);
        }

        QRect plotRect() const
        {
            const QFontMetrics metrics = fontMetrics();
            const int labelHeight = metrics.height() + 4;
            const int xLabelWidth = qMax(50, metrics.horizontalAdvance(QStringLiteral("65535")) + 12);
            const int yLabelWidth = qMax(40, metrics.horizontalAdvance(QStringLiteral("999.9M")) + 8);
            return rect().adjusted(
                yLabelWidth + 6,
                labelHeight + 2,
                -(xLabelWidth / 2 + 4),
                -(2 * labelHeight + 8));
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QPalette& colors = palette();
            const QFontMetrics metrics = painter.fontMetrics();
            const int labelHeight = metrics.height() + 4;
            const int xLabelWidth = qMax(50, metrics.horizontalAdvance(QStringLiteral("65535")) + 12);
            const QRect plot = plotRect();

            painter.fillRect(plot, colors.brush(QPalette::Base));
            painter.setPen(QPen(colors.color(QPalette::Mid), 1));
            painter.drawRect(plot);

            if (!m_stats.hasData() || m_stats.histogram.empty())
            {
                painter.setPen(colors.color(QPalette::PlaceholderText));
                painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No Histogram Data"));
                return;
            }

            int maxCount = 0;
            for (const int count : m_stats.histogram)
            {
                maxCount = qMax(maxCount, count);
            }
            if (maxCount == 0)
            {
                painter.setPen(colors.color(QPalette::PlaceholderText));
                painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No Histogram Data"));
                return;
            }

            painter.setPen(QPen(colors.color(QPalette::Highlight), 1));
            const int histogramSize = static_cast<int>(m_stats.histogram.size());
            for (int i = 0; i < histogramSize; ++i)
            {
                const int count = m_stats.histogram[static_cast<size_t>(i)];
                const double normalized = m_logScale && count > 0
                                              ? log10(count + 1.0) / log10(maxCount + 1.0)
                                              : static_cast<double>(count) / maxCount;
                const int x = plot.left() + (i * plot.width()) / histogramSize;
                const int height = static_cast<int>(normalized * plot.height());
                painter.drawLine(x, plot.bottom(), x, plot.bottom() - height);
            }

            const int domain = qMax(1, m_domainMax);
            const int xMin = qBound(plot.left(), plot.left() + static_cast<int>(static_cast<qint64>(m_minLevel) * plot.width() / domain), plot.right());
            const int xMax = qBound(plot.left(), plot.left() + static_cast<int>(static_cast<qint64>(m_maxLevel) * plot.width() / domain), plot.right());

            if (xMin > plot.left())
            {
                painter.fillRect(QRect(plot.left(), plot.top(), xMin - plot.left(), plot.height()), QColor(0, 0, 0, 70));
            }
            if (xMax < plot.right())
            {
                painter.fillRect(QRect(xMax, plot.top(), plot.right() - xMax, plot.height()), QColor(0, 0, 0, 70));
            }

            painter.setPen(QPen(QColor(0, 200, 255), 2));
            painter.drawLine(xMin, plot.top(), xMin, plot.bottom());
            QPolygon minHandle;
            minHandle << QPoint(xMin - 4, plot.top()) << QPoint(xMin + 4, plot.top()) << QPoint(xMin, plot.top() + 6);
            painter.setBrush(QColor(0, 200, 255));
            painter.drawPolygon(minHandle);

            painter.setPen(QPen(QColor(255, 180, 0), 2));
            painter.drawLine(xMax, plot.top(), xMax, plot.bottom());
            QPolygon maxHandle;
            maxHandle << QPoint(xMax - 4, plot.top()) << QPoint(xMax + 4, plot.top()) << QPoint(xMax, plot.top() + 6);
            painter.setBrush(QColor(255, 180, 0));
            painter.drawPolygon(maxHandle);

            painter.setPen(QPen(colors.color(QPalette::Mid), 1));
            painter.drawLine(plot.left(), plot.top(), plot.left(), plot.bottom());
            painter.drawLine(plot.left(), plot.bottom(), plot.right(), plot.bottom());

            const int maxValue = qMax(1, m_stats.maxValue);
            for (int i = 0; i <= 4; ++i)
            {
                const int x = plot.left() + (i * plot.width()) / 4;
                const int value = (i * maxValue) / 4;
                painter.drawLine(x, plot.bottom(), x, plot.bottom() + 5);
                painter.setPen(colors.color(QPalette::Text));
                painter.drawText(QRect(x - xLabelWidth / 2,
                                       plot.bottom() + 5,
                                       xLabelWidth,
                                       labelHeight),
                                 Qt::AlignCenter,
                                 QString::number(value));
                painter.setPen(QPen(colors.color(QPalette::Mid), 1));
            }

            painter.setPen(colors.color(QPalette::Text));
            painter.drawText(QRect(plot.left(),
                                   plot.bottom() + labelHeight + 5,
                                   plot.width(),
                                   labelHeight),
                             Qt::AlignCenter,
                             QStringLiteral("Intensity"));
            painter.drawText(QRect(0,
                                   plot.top() - labelHeight,
                                   plot.left() - 8,
                                   labelHeight),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Count"));

            const QString readout = QStringLiteral("Min: %1  Max: %2").arg(m_minLevel).arg(m_maxLevel);
            painter.drawText(QRect(plot.left(), 0, plot.width(), labelHeight), Qt::AlignRight | Qt::AlignVCenter, readout);
        }

        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() != Qt::LeftButton)
            {
                QWidget::mousePressEvent(event);
                return;
            }
            const QRect plot = plotRect();
            if (plot.width() <= 0) return;
            const int domain = qMax(1, m_domainMax);
            const int xMin = plot.left() + static_cast<int>(static_cast<qint64>(m_minLevel) * plot.width() / domain);
            const int xMax = plot.left() + static_cast<int>(static_cast<qint64>(m_maxLevel) * plot.width() / domain);
            const int mx = event->pos().x();
            if (std::abs(mx - xMin) <= 8)
            {
                m_dragMode = DragMode::MinLevel;
            }
            else if (std::abs(mx - xMax) <= 8)
            {
                m_dragMode = DragMode::MaxLevel;
            }
            else if (std::abs(mx - xMin) < std::abs(mx - xMax))
            {
                m_dragMode = DragMode::MinLevel;
                updateLevelFromMouse(mx);
            }
            else
            {
                m_dragMode = DragMode::MaxLevel;
                updateLevelFromMouse(mx);
            }
            event->accept();
        }

        void mouseMoveEvent(QMouseEvent* event) override
        {
            const QRect plot = plotRect();
            if (m_dragMode != DragMode::None)
            {
                updateLevelFromMouse(event->pos().x());
                event->accept();
                return;
            }
            if (plot.width() > 0)
            {
                const int domain = qMax(1, m_domainMax);
                const int xMin = plot.left() + static_cast<int>(static_cast<qint64>(m_minLevel) * plot.width() / domain);
                const int xMax = plot.left() + static_cast<int>(static_cast<qint64>(m_maxLevel) * plot.width() / domain);
                const int mx = event->pos().x();
                if (std::abs(mx - xMin) <= 8 || std::abs(mx - xMax) <= 8)
                {
                    setCursor(Qt::SizeHorCursor);
                }
                else
                {
                    setCursor(Qt::ArrowCursor);
                }
            }
            QWidget::mouseMoveEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent* event) override
        {
            if (m_dragMode != DragMode::None)
            {
                m_dragMode = DragMode::None;
                event->accept();
                return;
            }
            QWidget::mouseReleaseEvent(event);
        }

        void mouseDoubleClickEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton)
            {
                if (m_autoLevelsCallback)
                {
                    m_autoLevelsCallback();
                }
                event->accept();
                return;
            }
            QWidget::mouseDoubleClickEvent(event);
        }

    private:
        enum class DragMode { None, MinLevel, MaxLevel };

        void updateLevelFromMouse(int mouseX)
        {
            const QRect plot = plotRect();
            if (plot.width() <= 0) return;
            const int domain = qMax(1, m_domainMax);
            const int rawVal = static_cast<int>(static_cast<qint64>(mouseX - plot.left()) * domain / plot.width());
            if (m_dragMode == DragMode::MinLevel)
            {
                const int newMin = qBound(0, rawVal, m_maxLevel - 1);
                if (newMin != m_minLevel)
                {
                    m_minLevel = newMin;
                    if (m_levelsCallback)
                    {
                        m_levelsCallback(m_minLevel, m_maxLevel);
                    }
                    update();
                }
            }
            else if (m_dragMode == DragMode::MaxLevel)
            {
                const int newMax = qBound(m_minLevel + 1, rawVal, domain);
                if (newMax != m_maxLevel)
                {
                    m_maxLevel = newMax;
                    if (m_levelsCallback)
                    {
                        m_levelsCallback(m_minLevel, m_maxLevel);
                    }
                    update();
                }
            }
        }

        scopeone::core::ScopeOneCore::HistogramStats m_stats;
        bool m_logScale{false};
        int m_minLevel{0};
        int m_maxLevel{255};
        int m_domainMax{255};
        DragMode m_dragMode{DragMode::None};
        LevelsCallback m_levelsCallback;
        AutoLevelsCallback m_autoLevelsCallback;
    };

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
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::deviceStateChanged,
                this, [this]()
                {
                    if (!m_scopeonecore->configurationOperationRunning())
                    {
                        refreshCameraParameters();
                    }
                },
                Qt::QueuedConnection);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::layerHistogramReady,
                this, &DeviceControlWidget::onLayerHistogramReady);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::layerAnalysisCleared,
                this, [this](const QString& layerKey)
                {
                    if (layerKey == currentLayerKey())
                    {
                        m_layerHistogramWidget->clear();
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::stagePositionChanged,
                this, [this]()
                {
                    if (!m_scopeonecore->configurationOperationRunning())
                    {
                        updateStagePositions();
                    }
                },
                Qt::QueuedConnection);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::stageMoveFinished,
                this,
                [this](quint64 commandId,
                       const QString& deviceLabel,
                       bool success,
                       const QString& errorMessage)
                {
                    if (!m_pendingStageMoveIds.remove(commandId) || success)
                    {
                        return;
                    }
                    emit stageMoveFailed(
                        errorMessage.isEmpty()
                            ? tr("Failed to move stage: %1").arg(deviceLabel)
                            : tr("Failed to move stage %1: %2").arg(deviceLabel, errorMessage));
                });
        updateControlsState();
        refreshStageDevices();
        m_currentTarget = m_cameraSelectCombo->currentText();
    }

    // Builds the device control layout
    void DeviceControlWidget::setupUI()
    {
        m_imageControlsWidget = new QScrollArea(this);
        m_imageControlsWidget->setWidgetResizable(true);
        m_imageControlsWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_imageControlsWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_imageControlsWidget->setFrameShape(QFrame::NoFrame);
        auto* imageContainer = new QWidget(m_imageControlsWidget);
        auto* imageLayout = new QVBoxLayout(imageContainer);
        imageLayout->setSpacing(5);
        imageLayout->setContentsMargins(5, 5, 5, 5);
        imageLayout->addWidget(createPreviewControlsGroup());
        imageLayout->addStretch();
        m_imageControlsWidget->setWidget(imageContainer);

        m_hardwareControlsWidget = new QScrollArea(this);
        m_hardwareControlsWidget->setWidgetResizable(true);
        m_hardwareControlsWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_hardwareControlsWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_hardwareControlsWidget->setFrameShape(QFrame::NoFrame);
        auto* hardwareContainer = new QWidget(m_hardwareControlsWidget);
        auto* hardwareLayout = new QVBoxLayout(hardwareContainer);
        hardwareLayout->setSpacing(5);
        hardwareLayout->setContentsMargins(5, 5, 5, 5);
        hardwareLayout->addWidget(createControlGroup());
        auto* stageGroup = createStageGroup();
        hardwareLayout->addWidget(stageGroup);
        hardwareLayout->addStretch();
        m_hardwareControlsWidget->setWidget(hardwareContainer);
    }

    void DeviceControlWidget::setImageWorkspace(ImageWorkspace* workspace)
    {
        m_workspace = workspace;
        connect(m_workspace, &ImageWorkspace::activeLayerChanged,
                this, [this](const QString&)
                {
                    syncLayerSelection();
                    refreshPreviewLayerSettings();
                    refreshLayerHistogram();
                    updateControlsState();
                    if (m_liveViewerContext)
                    {
                        syncControlTargetToSelectedRawLayer();
                    }
                });
        connect(m_workspace, &ImageWorkspace::activeFrameChanged,
                this, &DeviceControlWidget::refreshLayerHistogram);
        connect(m_workspace, &ImageWorkspace::activeViewerChanged,
                this, &DeviceControlWidget::refreshLayerHistogram);
        connect(m_workspace, &ImageWorkspace::histogramReady,
                this, &DeviceControlWidget::onLayerHistogramReady);
        syncLayerSelection();
        refreshLayerHistogram();
    }

    QWidget* DeviceControlWidget::imageControlsWidget() const
    {
        return m_imageControlsWidget;
    }

    QWidget* DeviceControlWidget::hardwareControlsWidget() const
    {
        return m_hardwareControlsWidget;
    }

    void DeviceControlWidget::setControlsEnabled(bool enabled)
    {
        m_imageControlsWidget->setEnabled(enabled);
        m_hardwareControlsWidget->setEnabled(enabled);
    }

    // Connects the preview widget to control panel state
    void DeviceControlWidget::setPreviewWidget(PreviewWidget* preview)
    {
        if (m_previewWidget)
        {
            disconnect(m_previewWidget, nullptr, this, nullptr);
        }
        if (m_sceneModel)
        {
            disconnect(m_sceneModel, nullptr, this, nullptr);
        }
        m_previewWidget = preview;
        m_sceneModel = preview->sceneModel();

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
        connect(m_previewWidget, &PreviewWidget::visibleLayerKeysChanged,
                this, [this](const QStringList& layerKeys)
                {
                    applyPreviewVisibility(layerKeys, false);
                });
        connect(m_previewWidget, &PreviewWidget::layerInfoTextChanged,
                this, &DeviceControlWidget::onPreviewLayerInfoTextChanged);
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layerDisplayChanged,
                this, [this](const QString& layerKey)
                {
                    if (layerKey == currentLayerKey())
                    {
                        refreshPreviewLayerSettings();
                    }
                });
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layerAutoStretchChanged,
                this, [this](const QString& layerKey, bool)
                {
                    if (layerKey == currentLayerKey())
                    {
                        refreshPreviewLayerSettings();
                    }
                });
        connect(m_sceneModel,
                &scopeone::core::ImageSceneModel::sourceDisplayTransformChanged,
                this, [this](const QString& sourceId)
                {
                    if (sourceId == selectedLayerSourceId())
                    {
                        refreshPreviewLayerSettings();
                    }
                });

        onPreviewAvailableCameraIdsChanged(m_previewWidget->availableCameraIds());
        onPreviewAvailableLayerKeysChanged(m_previewWidget->availableLayerKeys());
        applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
        onPreviewLayerInfoTextChanged(m_previewWidget->layerInfoSummaryText());

        if (m_clippingCheckBox)
        {
            m_clippingCheckBox->setChecked(m_previewWidget->isClippingWarningEnabled());
            connect(m_clippingCheckBox, &QCheckBox::toggled,
                    m_previewWidget, &PreviewWidget::setClippingWarningEnabled);
            connect(m_previewWidget, &PreviewWidget::clippingWarningChanged,
                    m_clippingCheckBox, &QCheckBox::setChecked);
        }
        if (m_scaleBarCheckBox)
        {
            m_scaleBarCheckBox->setChecked(m_previewWidget->isScaleBarVisible());
            connect(m_scaleBarCheckBox, &QCheckBox::toggled,
                    m_previewWidget, &PreviewWidget::setScaleBarVisible);
            connect(m_previewWidget, &PreviewWidget::scaleBarVisibilityChanged,
                    m_scaleBarCheckBox, &QCheckBox::setChecked);
        }
        {
            const QSignalBlocker blocker(m_viewDimensionCombo);
            m_viewDimensionCombo->setCurrentIndex(
                m_previewWidget->viewDimensionMode() == PreviewWidget::ViewDimensionMode::ThreeDimensional
                    ? 1
                    : 0);
        }
        {
            const QSignalBlocker blocker(m_3dZScaleSlider);
            m_3dZScaleSlider->setValue(qRound(m_previewWidget->get3dZScale() * 10.0f));
        }
        {
            const QSignalBlocker blocker(m_3dZScaleSpinBox);
            m_3dZScaleSpinBox->setValue(m_previewWidget->get3dZScale());
        }
        {
            const QSignalBlocker blocker(m_3dWireframeCheckBox);
            m_3dWireframeCheckBox->setChecked(m_previewWidget->is3dWireframeEnabled());
        }
        connect(m_previewWidget, &PreviewWidget::viewDimensionModeChanged,
                this, [this](PreviewWidget::ViewDimensionMode mode)
                {
                    const QSignalBlocker blocker(m_viewDimensionCombo);
                    m_viewDimensionCombo->setCurrentIndex(
                        mode == PreviewWidget::ViewDimensionMode::ThreeDimensional ? 1 : 0);
                });
        connect(m_previewWidget, &PreviewWidget::threeDimensionalZScaleChanged,
                this, [this](float scale)
                {
                    {
                        const QSignalBlocker sliderBlocker(m_3dZScaleSlider);
                        m_3dZScaleSlider->setValue(qRound(scale * 10.0f));
                    }
                    const QSignalBlocker spinBlocker(m_3dZScaleSpinBox);
                    m_3dZScaleSpinBox->setValue(scale);
                });
        connect(m_previewWidget, &PreviewWidget::threeDimensionalWireframeChanged,
                this, [this](bool enabled)
                {
                    const QSignalBlocker blocker(m_3dWireframeCheckBox);
                    m_3dWireframeCheckBox->setChecked(enabled);
                });
    }

    // Builds layer and alignment controls
    QWidget* DeviceControlWidget::createPreviewControlsGroup()
    {
        m_previewControlsGroup = new QGroupBox("Layers", this);
        QGridLayout* controlLayout = new QGridLayout(m_previewControlsGroup);
        controlLayout->setHorizontalSpacing(6);
        controlLayout->setVerticalSpacing(4);
        controlLayout->setContentsMargins(6, 6, 6, 6);

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

        m_layerHistogramGroup = new QGroupBox(QStringLiteral("Histogram"), m_previewControlsGroup);
        auto* histogramLayout = new QVBoxLayout(m_layerHistogramGroup);
        histogramLayout->setContentsMargins(6, 6, 6, 6);
        m_layerHistogramWidget = new LayerHistogramWidget(m_layerHistogramGroup);
        histogramLayout->addWidget(m_layerHistogramWidget);
        auto* histogramLevelsLayout = new QHBoxLayout;
        m_layerAutoButton = new QPushButton(QStringLiteral("Auto"), m_layerHistogramGroup);
        m_layerAutoButton->setToolTip(QStringLiteral("Set display levels from the current image"));
        m_layerFullRangeButton = new QPushButton(QStringLiteral("Full Range"), m_layerHistogramGroup);
        m_layerFullRangeButton->setToolTip(QStringLiteral("Show the complete intensity range"));
        histogramLevelsLayout->addWidget(m_layerAutoButton);
        histogramLevelsLayout->addWidget(m_layerFullRangeButton);
        histogramLayout->addLayout(histogramLevelsLayout);

        m_layerAutoStretchCheckBox = new QCheckBox(QStringLiteral("Continuous Auto"), m_layerHistogramGroup);
        m_layerAutoStretchCheckBox->setToolTip(
            QStringLiteral("Update display levels continuously as images arrive"));

        m_layerHistogramLogCheckBox = new QCheckBox(QStringLiteral("Log scale"), m_layerHistogramGroup);
        histogramLayout->addWidget(m_layerAutoStretchCheckBox);
        histogramLayout->addWidget(m_layerHistogramLogCheckBox);
        connect(m_layerHistogramLogCheckBox, &QCheckBox::toggled,
                m_layerHistogramWidget, &LayerHistogramWidget::setLogScale);

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
        m_layerImportButton = new QPushButton(QStringLiteral("Import..."), m_layerSettingsGroup);
        m_layerImportButton->setMaximumWidth(68);

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
        m_clippingCheckBox = new QCheckBox(QStringLiteral("Hi-Lo Warn"), m_layerSettingsGroup);
        m_clippingCheckBox->setToolTip(QStringLiteral("Highlight saturated pixels in red and zero pixels in blue (Hotkey: C)"));
        m_scaleBarCheckBox = new QCheckBox(QStringLiteral("Scale Bar"), m_layerSettingsGroup);
        m_scaleBarCheckBox->setToolTip(QStringLiteral("Display calibrated scale bar in viewport"));
        m_scaleBarCheckBox->setChecked(true);

        layerSettingsLayout->addWidget(m_selectedLayerLabel, 0, 0, 1, 6);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Order:"), m_layerSettingsGroup), 1, 0);
        layerSettingsLayout->addWidget(m_layerMoveUpButton, 1, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_layerMoveDownButton, 1, 2, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_layerRemoveButton, 1, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(m_layerImportButton, 1, 4, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Opacity:"), m_layerSettingsGroup), 2, 0);
        layerSettingsLayout->addWidget(m_layerOpacitySpinBox, 2, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Gamma:"), m_layerSettingsGroup), 2, 2);
        layerSettingsLayout->addWidget(m_layerGammaSpinBox, 2, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Color:"), m_layerSettingsGroup), 3, 0);
        layerSettingsLayout->addWidget(m_layerColormapComboBox, 3, 1);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Blend:"), m_layerSettingsGroup), 3, 2);
        layerSettingsLayout->addWidget(m_layerBlendingComboBox, 3, 3, 1, 3);
        layerSettingsLayout->addWidget(m_clippingCheckBox, 4, 0, 1, 3);
        layerSettingsLayout->addWidget(m_scaleBarCheckBox, 4, 3, 1, 3);

        auto* transformGroup = new QGroupBox(QStringLiteral("Transform"), this);
        auto* transformLayout = new QGridLayout(transformGroup);
        transformLayout->setContentsMargins(6, 6, 6, 6);
        m_alignXLabel = new QLabel("X offset:", transformGroup);
        m_alignXSpinBox = new QSpinBox(transformGroup);
        m_alignXSpinBox->setRange(-1000, 1000);
        m_alignXSpinBox->setValue(0);
        m_alignXSpinBox->setFixedWidth(64);
        m_alignXSpinBox->setKeyboardTracking(false);

        m_alignYLabel = new QLabel("Y offset:", transformGroup);
        m_alignYSpinBox = new QSpinBox(transformGroup);
        m_alignYSpinBox->setRange(-1000, 1000);
        m_alignYSpinBox->setValue(0);
        m_alignYSpinBox->setFixedWidth(64);
        m_alignYSpinBox->setKeyboardTracking(false);

        m_alignZoomLabel = new QLabel("Scale:", transformGroup);
        m_alignZoomSpinBox = new QSpinBox(transformGroup);
        m_alignZoomSpinBox->setRange(10, 500);
        m_alignZoomSpinBox->setValue(100);
        m_alignZoomSpinBox->setFixedWidth(58);
        m_alignZoomSpinBox->setToolTip("Source camera display scale percent");
        m_alignZoomSpinBox->setKeyboardTracking(false);

        m_alignFlipXCheckBox = new QCheckBox("Flip X", transformGroup);
        m_alignFlipYCheckBox = new QCheckBox("Flip Y", transformGroup);
        m_alignResetButton = new QPushButton("Reset", transformGroup);
        m_alignResetButton->setMaximumWidth(50);
        m_alignResetButton->setToolTip("Reset offset and flip");

        transformLayout->addWidget(m_alignXLabel, 0, 0);
        transformLayout->addWidget(m_alignXSpinBox, 0, 1, Qt::AlignLeft);
        transformLayout->addWidget(m_alignYLabel, 0, 2);
        transformLayout->addWidget(m_alignYSpinBox, 0, 3, Qt::AlignLeft);
        transformLayout->addWidget(m_alignZoomLabel, 1, 0);
        transformLayout->addWidget(m_alignZoomSpinBox, 1, 1, Qt::AlignLeft);
        transformLayout->addWidget(m_alignFlipXCheckBox, 1, 2, Qt::AlignLeft);
        transformLayout->addWidget(m_alignFlipYCheckBox, 1, 3, Qt::AlignLeft);
        transformLayout->addWidget(m_alignResetButton, 1, 4, Qt::AlignLeft);
        transformLayout->setColumnStretch(5, 1);

        m_alignXLabel->setMinimumWidth(20);
        m_alignYLabel->setMinimumWidth(20);
        m_alignZoomLabel->setMinimumWidth(60);

        m_surfaceViewGroup = new QGroupBox(QStringLiteral("Surface View"), this);
        auto* surfaceViewLayout = new QGridLayout(m_surfaceViewGroup);
        surfaceViewLayout->setContentsMargins(6, 6, 6, 6);
        surfaceViewLayout->setHorizontalSpacing(6);
        surfaceViewLayout->setVerticalSpacing(4);

        m_viewDimensionCombo = new QComboBox(m_surfaceViewGroup);
        m_viewDimensionCombo->addItem(QStringLiteral("2D Flat Map"));
        m_viewDimensionCombo->addItem(QStringLiteral("3D Surface"));
        m_viewDimensionCombo->setToolTip(QStringLiteral("Switch between the flat image and a displaced surface"));

        m_3dZScaleSlider = new QSlider(Qt::Horizontal, m_surfaceViewGroup);
        m_3dZScaleSlider->setRange(1, 100);
        m_3dZScaleSlider->setValue(10);
        m_3dZScaleSlider->setToolTip(QStringLiteral("Height exaggeration from 0.1x to 10.0x"));

        m_3dZScaleSpinBox = new QDoubleSpinBox(m_surfaceViewGroup);
        m_3dZScaleSpinBox->setRange(0.1, 10.0);
        m_3dZScaleSpinBox->setSingleStep(0.1);
        m_3dZScaleSpinBox->setDecimals(1);
        m_3dZScaleSpinBox->setSuffix(QStringLiteral("x"));
        m_3dZScaleSpinBox->setValue(1.0);
        m_3dZScaleSpinBox->setFixedWidth(64);
        m_3dZScaleSpinBox->setKeyboardTracking(false);

        m_3dWireframeCheckBox = new QCheckBox(QStringLiteral("Wireframe"), m_surfaceViewGroup);
        m_3dResetButton = new QPushButton(QStringLiteral("Reset View"), m_surfaceViewGroup);
        m_3dResetButton->setMaximumWidth(84);

        surfaceViewLayout->addWidget(new QLabel(QStringLiteral("Mode:"), m_surfaceViewGroup), 0, 0);
        surfaceViewLayout->addWidget(m_viewDimensionCombo, 0, 1, 1, 3);
        surfaceViewLayout->addWidget(new QLabel(QStringLiteral("Z-Scale:"), m_surfaceViewGroup), 1, 0);
        surfaceViewLayout->addWidget(m_3dZScaleSlider, 1, 1);
        surfaceViewLayout->addWidget(m_3dZScaleSpinBox, 1, 2);
        surfaceViewLayout->addWidget(m_3dWireframeCheckBox, 2, 0, 1, 2);
        surfaceViewLayout->addWidget(m_3dResetButton, 2, 2, 1, 2, Qt::AlignLeft);

        connect(m_viewDimensionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index)
                {
                    m_previewWidget->setViewDimensionMode(
                        index == 1 ? PreviewWidget::ViewDimensionMode::ThreeDimensional
                                   : PreviewWidget::ViewDimensionMode::TwoDimensional);
                });
        connect(m_3dZScaleSlider, &QSlider::valueChanged, this,
                [this](int value)
                {
                    const float scale = static_cast<float>(value) / 10.0f;
                    {
                        const QSignalBlocker blocker(m_3dZScaleSpinBox);
                        m_3dZScaleSpinBox->setValue(scale);
                    }
                    m_previewWidget->set3dZScale(scale);
                });
        connect(m_3dZScaleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double value)
                {
                    {
                        const QSignalBlocker blocker(m_3dZScaleSlider);
                        m_3dZScaleSlider->setValue(qRound(value * 10.0));
                    }
                    m_previewWidget->set3dZScale(static_cast<float>(value));
                });
        connect(m_3dWireframeCheckBox, &QCheckBox::toggled,
                this, [this](bool enabled) { m_previewWidget->set3dWireframeEnabled(enabled); });
        connect(m_3dResetButton, &QPushButton::clicked,
                this, [this]() { m_previewWidget->reset3dCamera(); });

        controlLayout->addWidget(m_layerTable, 0, 0, 1, 6);
        controlLayout->addWidget(m_layerHistogramGroup, 1, 0, 1, 6);
        controlLayout->addWidget(m_layerSettingsGroup, 2, 0, 1, 6);
        controlLayout->addWidget(transformGroup, 3, 0, 1, 6);
        controlLayout->addWidget(m_surfaceViewGroup, 4, 0, 1, 6);

        controlLayout->setColumnStretch(5, 1);

        connect(m_layerTable, &QTableWidget::currentCellChanged,
                this, &DeviceControlWidget::onPreviewLayerSelectionChanged);
        connect(m_layerMoveUpButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerMoveUpClicked);
        connect(m_layerMoveDownButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerMoveDownClicked);
        connect(m_layerRemoveButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerRemoveClicked);
        connect(m_layerImportButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerImportClicked);
        connect(m_layerOpacitySpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerOpacityChanged);
        connect(m_layerGammaSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerGammaChanged);
        connect(m_layerColormapComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerColormapChanged);
        connect(m_layerBlendingComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerBlendingChanged);
        connect(m_layerAutoButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerAutoClicked);
        connect(m_layerFullRangeButton, &QPushButton::clicked,
                this, &DeviceControlWidget::onPreviewLayerFullRangeClicked);
        connect(m_layerAutoStretchCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onPreviewLayerAutoStretchToggled);
        m_layerHistogramWidget->setOnLevelsChanged([this](int minLevel, int maxLevel)
        {
            const QString layerKey = currentLayerKey();
            if (layerKey.isEmpty() || !m_sceneModel) return;
            scopeone::core::DocumentLayer layer;
            if (m_sceneModel->findLayer(layerKey, layer))
            {
                const int domainMax = layer.display.levelDomainMax > 0 ? layer.display.levelDomainMax : 255;
                if (m_workspace)
                {
                    m_workspace->setLayerAutoStretchEnabled(layerKey, false);
                }
                m_layerAutoStretchCheckBox->setChecked(false);
                m_sceneModel->setLayerDisplayLevels(layerKey, minLevel, maxLevel, domainMax);
            }
        });
        m_layerHistogramWidget->setOnAutoLevelsRequested([this]()
        {
            onPreviewLayerAutoClicked();
        });
        const auto applySourceTransform = [this]()
        {
            const QString sourceId = selectedLayerSourceId();
            if (sourceId.isEmpty())
            {
                return;
            }
            scopeone::core::ImageSceneModel::SourceDisplayTransform transform;
            transform.offsetX = m_alignXSpinBox->value();
            transform.offsetY = m_alignYSpinBox->value();
            transform.zoomPercent = m_alignZoomSpinBox->value();
            transform.flipX = m_alignFlipXCheckBox->isChecked();
            transform.flipY = m_alignFlipYCheckBox->isChecked();
            m_sceneModel->setSourceDisplayTransform(sourceId, transform);
        };
        connect(m_alignXSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [applySourceTransform](int) { applySourceTransform(); });
        connect(m_alignYSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [applySourceTransform](int) { applySourceTransform(); });
        connect(m_alignZoomSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [applySourceTransform](int) { applySourceTransform(); });
        connect(m_alignFlipXCheckBox, &QCheckBox::toggled,
                this, [applySourceTransform](bool) { applySourceTransform(); });
        connect(m_alignFlipYCheckBox, &QCheckBox::toggled,
                this, [applySourceTransform](bool) { applySourceTransform(); });
        connect(m_alignResetButton, &QPushButton::clicked,
                this, [this]()
                {
                    resetSelectedLayerTransform();
                });

        refreshPreviewLayerSettings();
        return m_previewControlsGroup;
    }

    // Rebuilds the preview layer table
    void DeviceControlWidget::rebuildPreviewLayerTable(const QStringList& layerKeys)
    {
        const QString previousLayerKey = currentLayerKey();
        QSignalBlocker tableBlocker(m_layerTable);
        m_layerRows.clear();
        m_layerTable->setRowCount(layerKeys.size());
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
        }

        const QString selectedLayerKey = selectedRow >= 0
                                             ? layerKeys.at(selectedRow)
                                             : QString{};
        if (m_workspace && selectedLayerKey != previousLayerKey)
        {
            m_workspace->setActiveLayerKey(selectedLayerKey);
        }
        refreshPreviewLayerSettings();
        syncControlTargetToSelectedRawLayer();
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

    // Applies layer visibility to layer rows and preview
    void DeviceControlWidget::applyPreviewVisibility(const QStringList& layerKeys, bool notifyPreview)
    {
        QSet<QString> visibleLayerKeySet;
        for (const QString& layerKey : layerKeys)
        {
            visibleLayerKeySet.insert(layerKey);
        }

        for (auto it = m_layerRows.begin(); it != m_layerRows.end(); ++it)
        {
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(visibleLayerKeySet.contains(it.key()));
        }

        if (notifyPreview)
        {
            m_sceneModel->setVisibleLayers(layerKeys);
        }

        const QString selectedLayerKey = currentLayerKey();
        if (!layerKeys.isEmpty() && !visibleLayerKeySet.contains(selectedLayerKey))
        {
            const QString nextLayerKey = layerKeys.first();
            for (int row = 0; row < m_layerTable->rowCount(); ++row)
            {
                QTableWidgetItem* item = m_layerTable->item(row, 1);
                if (item->data(Qt::UserRole).toString() == nextLayerKey)
                {
                    QSignalBlocker tableBlocker(m_layerTable);
                    m_layerTable->setCurrentCell(row, 1);
                    if (m_workspace)
                    {
                        m_workspace->setActiveLayerKey(nextLayerKey);
                    }
                    break;
                }
            }
        }

        refreshPreviewLayerSettings();
    }

    // Updates the settings editor for the selected preview layer
    void DeviceControlWidget::refreshPreviewLayerSettings()
    {
        const QString layerKey = currentLayerKey();
        const bool hasLayer = !layerKey.isEmpty() && m_layerRows.contains(layerKey);
        m_layerSettingsGroup->setEnabled(hasLayer);
        m_layerAutoButton->setEnabled(hasLayer);
        m_layerFullRangeButton->setEnabled(hasLayer);
        m_layerAutoStretchCheckBox->setEnabled(hasLayer);
        if (!hasLayer)
        {
            m_selectedLayerLabel->setText(QStringLiteral("No layer selected"));
            return;
        }

        m_selectedLayerLabel->setText(m_previewWidget->layerName(layerKey));

        scopeone::core::DocumentLayer layer;
        m_sceneModel->findLayer(layerKey, layer);
        {
            QSignalBlocker blocker(m_layerOpacitySpinBox);
            m_layerOpacitySpinBox->setValue(layer.display.opacityPercent);
        }
        {
            QSignalBlocker blocker(m_layerGammaSpinBox);
            m_layerGammaSpinBox->setValue(layer.display.gamma);
        }
        {
            QSignalBlocker blocker(m_layerColormapComboBox);
            const int index = m_layerColormapComboBox->findText(layer.display.colormap);
            m_layerColormapComboBox->setCurrentIndex(index);
        }
        {
            QSignalBlocker blocker(m_layerBlendingComboBox);
            const int index = m_layerBlendingComboBox->findText(layer.display.blending);
            m_layerBlendingComboBox->setCurrentIndex(index);
        }
        {
            QSignalBlocker blocker(m_layerAutoStretchCheckBox);
            m_layerAutoStretchCheckBox->setChecked(
                m_workspace->layerAutoStretchEnabled(layerKey));
        }

        m_layerHistogramWidget->setLevels(
            layer.display.levelMin, layer.display.levelMax, layer.display.levelDomainMax);

        const int row = m_layerTable->currentRow();
        m_layerMoveUpButton->setEnabled(row > 0);
        m_layerMoveDownButton->setEnabled(row < m_layerTable->rowCount() - 1);
        m_layerRemoveButton->setEnabled(
            m_sceneModel == m_scopeonecore->imageSceneModel()
            && scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey));
        m_layerOpacitySpinBox->setEnabled(m_layerBlendingComboBox->currentText() != QStringLiteral("Opaque"));

        const QString sourceId = selectedLayerSourceId();
        const scopeone::core::ImageSceneModel::SourceDisplayTransform transform =
            m_sceneModel->sourceDisplayTransform(sourceId);
        {
            QSignalBlocker blocker(m_alignXSpinBox);
            m_alignXSpinBox->setValue(transform.offsetX);
        }
        {
            QSignalBlocker blocker(m_alignYSpinBox);
            m_alignYSpinBox->setValue(transform.offsetY);
        }
        {
            QSignalBlocker blocker(m_alignZoomSpinBox);
            m_alignZoomSpinBox->setValue(transform.zoomPercent);
        }
        {
            QSignalBlocker blocker(m_alignFlipXCheckBox);
            m_alignFlipXCheckBox->setChecked(transform.flipX);
        }
        {
            QSignalBlocker blocker(m_alignFlipYCheckBox);
            m_alignFlipYCheckBox->setChecked(transform.flipY);
        }
    }

    QString DeviceControlWidget::selectedLayerSourceId() const
    {
        return scopeone::core::ScopeOneCore::sourceIdFromLayerKey(currentLayerKey());
    }

    QString DeviceControlWidget::currentLayerKey() const
    {
        return m_workspace ? m_workspace->activeLayerKey() : QString{};
    }

    void DeviceControlWidget::syncLayerSelection()
    {
        const QString layerKey = currentLayerKey();
        for (int row = 0; row < m_layerTable->rowCount(); ++row)
        {
            QTableWidgetItem* item = m_layerTable->item(row, 1);
            if (item && item->data(Qt::UserRole).toString() == layerKey)
            {
                const QSignalBlocker blocker(m_layerTable);
                m_layerTable->setCurrentCell(row, 1);
                refreshPreviewLayerSettings();
                return;
            }
        }
        const QSignalBlocker blocker(m_layerTable);
        m_layerTable->clearSelection();
        refreshPreviewLayerSettings();
    }

    // Refreshes selected layer transform values when live cameras change
    void DeviceControlWidget::onPreviewAvailableCameraIdsChanged(const QStringList&)
    {
        refreshPreviewLayerSettings();
    }

    void DeviceControlWidget::onPreviewAvailableLayerKeysChanged(const QStringList& layerKeys)
    {
        rebuildPreviewLayerTable(layerKeys);
        applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
        refreshLayerHistogram();
        updateControlsState();
    }

    void DeviceControlWidget::onPreviewLayerInfoTextChanged(const QString&)
    {
        refreshPreviewLayerInfoText();
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
        m_sceneModel->setLayerVisible(layerKey, checkBox->isChecked());
    }

    void DeviceControlWidget::onPreviewLayerOpacityChanged(int value)
    {
        m_sceneModel->setLayerOpacityPercent(currentLayerKey(), value);
    }

    void DeviceControlWidget::onPreviewLayerGammaChanged(double value)
    {
        m_sceneModel->setLayerGamma(currentLayerKey(), value);
    }

    void DeviceControlWidget::onPreviewLayerColormapChanged(int)
    {
        m_sceneModel->setLayerColormap(
            currentLayerKey(), m_layerColormapComboBox->currentText());
    }

    void DeviceControlWidget::onPreviewLayerBlendingChanged(int)
    {
        m_sceneModel->setLayerBlending(
            currentLayerKey(), m_layerBlendingComboBox->currentText());
        m_layerOpacitySpinBox->setEnabled(m_layerBlendingComboBox->currentText() != QStringLiteral("Opaque"));
    }

    void DeviceControlWidget::onPreviewLayerAutoClicked()
    {
        m_workspace->autoLayerLevels(currentLayerKey());
    }

    void DeviceControlWidget::onPreviewLayerFullRangeClicked()
    {
        m_workspace->fullLayerLevels(currentLayerKey());
    }

    void DeviceControlWidget::onPreviewLayerAutoStretchToggled(bool enabled)
    {
        m_workspace->setLayerAutoStretchEnabled(currentLayerKey(), enabled);
    }

    void DeviceControlWidget::onPreviewLayerSelectionChanged(int currentRow, int, int, int)
    {
        QTableWidgetItem* item = m_layerTable->item(currentRow, 1);
        const QString layerKey = item ? item->data(Qt::UserRole).toString() : QString();
        if (m_workspace)
        {
            m_workspace->setActiveLayerKey(layerKey);
        }
        refreshPreviewLayerSettings();
        refreshLayerHistogram();
        updateControlsState();
    }

    void DeviceControlWidget::onPreviewLayerMoveUpClicked()
    {
        m_sceneModel->moveLayer(currentLayerKey(), -1);
    }

    void DeviceControlWidget::onPreviewLayerMoveDownClicked()
    {
        m_sceneModel->moveLayer(currentLayerKey(), 1);
    }

    void DeviceControlWidget::onPreviewLayerRemoveClicked()
    {
        m_scopeonecore->removeStaticFrame(
            scopeone::core::ScopeOneCore::sourceIdFromLayerKey(currentLayerKey()));
    }

    // Open file dialog and import image files as static layers
    void DeviceControlWidget::onPreviewLayerImportClicked()
    {
        const QStringList filePaths = QFileDialog::getOpenFileNames(
            this,
            tr("Import Image as Layer"),
            QString(),
            tr("Images (*.tif *.tiff *.png *.jpg *.jpeg *.bmp)"));
        QString lastLayerKey;
        for (const QString& filePath : filePaths)
        {
            m_scopeonecore->importImageAsStaticLayer(filePath, &lastLayerKey);
        }
        if (!lastLayerKey.isEmpty() && m_workspace)
        {
            m_workspace->setActiveLayerKey(lastLayerKey);
        }
    }

    void DeviceControlWidget::onLayerHistogramReady(
        const QString& layerKey,
        const scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        if (layerKey == currentLayerKey())
        {
            m_layerHistogramWidget->setStats(stats);
        }
    }

    void DeviceControlWidget::refreshLayerHistogram()
    {
        const QString layerKey = currentLayerKey();
        if (layerKey.isEmpty())
        {
            m_layerHistogramWidget->clear();
            return;
        }

        if (m_workspace->isLiveViewerActive())
        {
            scopeone::core::ScopeOneCore::HistogramStats stats;
            if (m_scopeonecore->getLayerHistogram(layerKey, stats))
            {
                m_layerHistogramWidget->setStats(stats);
            }
            else
            {
                m_layerHistogramWidget->clear();
            }
            m_scopeonecore->setActiveHistogramLayer(layerKey);
            return;
        }

        m_layerHistogramWidget->clear();
        m_workspace->requestHistogram(layerKey);
    }

    // Use the selected raw camera layer as the hardware control target
    void DeviceControlWidget::syncControlTargetToSelectedRawLayer()
    {
        if (isAllTarget(m_currentTarget)
            || !scopeone::core::ScopeOneCore::isRawLayerKey(currentLayerKey()))
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
            m_sceneModel->resetSourceDisplayTransform(sourceId);
        }
    }

    QWidget* DeviceControlWidget::createControlGroup()
    {
        QGroupBox* group = new QGroupBox("Camera Controls");
        m_cameraControlsGroup = group;
        QGridLayout* layout = new QGridLayout(group);

        int row = 0;

        layout->addWidget(new QLabel("Control Target:"), row, 0);
        m_cameraSelectCombo = new QComboBox();
        m_cameraSelectCombo->addItem("All");
        connect(m_cameraSelectCombo, &QComboBox::currentTextChanged,
                this, &DeviceControlWidget::onControlTargetSelectionChanged);
        layout->addWidget(m_cameraSelectCombo, row, 1);
        row++;

        m_exposureLabel = new QLabel("Exposure (ms):", group);
        layout->addWidget(m_exposureLabel, row, 0);
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
        m_previewToggleButton->setMinimumHeight(30);
        connect(m_previewToggleButton, &QPushButton::clicked, this, &DeviceControlWidget::onPreviewToggleClicked);
        m_snapButton = new QPushButton("Snap", group);
        m_snapButton->setMinimumHeight(30);
        m_snapButton->setToolTip(tr("Capture the latest frame from the selected target"));
        connect(m_snapButton, &QPushButton::clicked, this, [this]()
        {
            emit snapRequested(m_currentTarget);
        });
        layout->addWidget(m_previewToggleButton, row, 0);
        layout->addWidget(m_snapButton, row, 1);

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
        m_stageControlsGroup = group;
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
        const bool hasXY = m_liveViewerContext && !selectedXYStageLabel().isEmpty();
        const bool hasZ = m_liveViewerContext && !selectedZStageLabel().isEmpty();

        m_xyStageCombo->setEnabled(m_liveViewerContext && m_xyStageCombo->count() > 0);
        m_zStageCombo->setEnabled(m_liveViewerContext && m_zStageCombo->count() > 0);
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
        const quint64 commandId = m_scopeonecore->moveXYRelative(xyLabel, dx, dy);
        if (commandId != 0)
        {
            m_pendingStageMoveIds.insert(commandId);
            return;
        }
        emit stageMoveFailed(tr("Failed to queue XY stage move: %1").arg(xyLabel));
    }

    // Moves the selected Z stage by a relative offset
    void DeviceControlWidget::moveZStage(double dz)
    {
        const QString zLabel = selectedZStageLabel();
        if (zLabel.isEmpty())
        {
            return;
        }
        const quint64 commandId = m_scopeonecore->moveZRelative(zLabel, dz);
        if (commandId != 0)
        {
            m_pendingStageMoveIds.insert(commandId);
            return;
        }
        emit stageMoveFailed(tr("Failed to queue Z stage move: %1").arg(zLabel));
    }

    // Moves XY stage with step factor
    void DeviceControlWidget::moveXYStep(double dxScale, double dyScale, bool big)
    {
        const double stepValue = (big ? m_xyBigStepLineEdit : m_xyStepLineEdit)->text().toDouble();
        if (stepValue > 0.0)
        {
            moveXYStage(dxScale * stepValue, dyScale * stepValue);
        }
    }

    // Moves Z stage with step factor
    void DeviceControlWidget::moveZStep(double dzScale, bool big)
    {
        const double stepValue = (big ? m_zBigStepLineEdit : m_zStepLineEdit)->text().toDouble();
        if (stepValue > 0.0)
        {
            moveZStage(dzScale * stepValue);
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
        const bool canControlHardware = m_cameraInitialized && m_liveViewerContext;
        m_cameraControlsGroup->setVisible(true);
        m_stageControlsGroup->setVisible(true);
        m_cameraControlsGroup->setEnabled(canControlHardware);
        m_stageControlsGroup->setEnabled(m_liveViewerContext);
        m_cameraSelectCombo->setEnabled(canControlHardware && m_controlTargetEnabled);
        m_exposureLineEdit->setEnabled(canControlHardware);
        m_previewToggleButton->setEnabled(canControlHardware);
        m_snapButton->setEnabled(canControlHardware);
        m_previewToggleButton->setText(m_previewRunning
                                           ? QStringLiteral("Stop Preview")
                                           : QStringLiteral("Start Preview"));
        const bool hasRoiTarget = !roiCameraTarget().isEmpty();
        m_drawROIButton->setEnabled(canControlHardware && hasRoiTarget);
        m_halfROIButton->setEnabled(canControlHardware && hasRoiTarget);
        m_clearROIButton->setEnabled(canControlHardware && hasRoiTarget);

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
            m_exposureLabel->setText(tr("Exposure (ms):"));
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
        m_exposureLabel->setText(tr("Exposure (ms):"));
    }

    // Updates preview running state for the control button
    void DeviceControlWidget::setPreviewRunning(bool running)
    {
        m_previewRunning = running;
        updateControlsState();
    }

    // Checks whether a control operation targets every camera
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
        {
            QSignalBlocker blocker(m_cameraSelectCombo);
            m_cameraSelectCombo->clear();
            m_cameraSelectCombo->addItem("All");
            for (const QString& id : cameraIds)
            {
                m_cameraSelectCombo->addItem(id);
            }

            if (cameraIds.size() > 1)
            {
                m_cameraSelectCombo->setCurrentIndex(0);
            }
            else if (cameraIds.size() == 1)
            {
                m_cameraSelectCombo->setCurrentIndex(1);
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
        m_controlTargetEnabled = enabled;
        updateControlsState();
    }

    void DeviceControlWidget::setViewerContext(bool liveViewer)
    {
        m_liveViewerContext = liveViewer;
        updateControlsState();
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
        const QString cameraId = roiCameraTarget();
        if (!cameraId.isEmpty())
        {
            emit requestClearROI(cameraId);
        }
    }
} // namespace scopeone::ui
