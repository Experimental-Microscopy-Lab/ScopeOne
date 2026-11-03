#include "DeviceControlWidget.h"
#include "ImageWorkspace.h"
#include "scopeone/ImageSceneModel.h"
#include "scopeone/ScopeOneCore.h"
#include "PreviewWidget.h"

#include <QAbstractItemView>
#include <QAction>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QIcon>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QShortcut>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scopeone::ui
{
    namespace
    {
        // Size a push button to its text instead of the style's minimum button width
        void fitButtonToText(QPushButton* button)
        {
            button->setFixedWidth(button->fontMetrics().horizontalAdvance(button->text()) + 20);
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

        // A line edit for a stage step in µm
        QLineEdit* makeStepLineEdit(const QString& text)
        {
            auto* lineEdit = new QLineEdit(text);
            lineEdit->setMinimumWidth(80);
            auto* validator = new QDoubleValidator(0.0, 100000.0, 5, lineEdit);
            validator->setLocale(QLocale::c());
            validator->setNotation(QDoubleValidator::StandardNotation);
            lineEdit->setValidator(validator);
            return lineEdit;
        }

        QScrollArea* makeScrollArea(QWidget* parent)
        {
            auto* area = new QScrollArea(parent);
            area->setWidgetResizable(true);
            area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            area->setFrameShape(QFrame::NoFrame);
            return area;
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
        // Quick choices of the display range, from the counts of the histogram
        void contextMenuEvent(QContextMenuEvent* event) override
        {
            if (!m_stats.hasData() || m_stats.histogram.empty())
            {
                return;
            }
            QMenu menu(this);
            QAction* tenth = menu.addAction(QStringLiteral("Saturate 0.1%"));
            QAction* one = menu.addAction(QStringLiteral("Saturate 1%"));
            QAction* extremes = menu.addAction(QStringLiteral("Minimum to Maximum"));
            QAction* full = menu.addAction(QStringLiteral("Full Range"));
            QAction* chosen = menu.exec(event->globalPos());
            if (!chosen)
            {
                return;
            }
            if (chosen == full)
            {
                m_levelsCallback(0, m_domainMax);
                return;
            }
            if (chosen == extremes)
            {
                m_levelsCallback(static_cast<int>(m_stats.minVal), static_cast<int>(m_stats.maxVal));
                return;
            }
            const double percent = chosen == tenth ? 0.1 : 1.0;
            const int size = static_cast<int>(m_stats.histogram.size());
            const qint64 total = std::accumulate(m_stats.histogram.begin(), m_stats.histogram.end(), qint64{0});
            const double limit = total * percent / 200.0;
            qint64 running = 0;
            int low = 0;
            for (int i = 0; i < size; ++i)
            {
                running += m_stats.histogram[static_cast<size_t>(i)];
                if (running > limit)
                {
                    low = i;
                    break;
                }
            }
            running = 0;
            int high = size - 1;
            for (int i = size - 1; i >= 0; --i)
            {
                running += m_stats.histogram[static_cast<size_t>(i)];
                if (running > limit)
                {
                    high = i;
                    break;
                }
            }
            const auto level = [this, size](int bin) { return size > 1 ? bin * m_domainMax / (size - 1) : 0; };
            m_levelsCallback(level(low), qMax(level(high), level(low) + 1));
        }

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

            const int maxCount = m_stats.hasData() && !m_stats.histogram.empty()
                                     ? *std::max_element(m_stats.histogram.begin(), m_stats.histogram.end())
                                     : 0;
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

            const int xMin = qBound(plot.left(), levelX(plot, m_minLevel), plot.right());
            const int xMax = qBound(plot.left(), levelX(plot, m_maxLevel), plot.right());

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
            const int xMin = levelX(plot, m_minLevel);
            const int xMax = levelX(plot, m_maxLevel);
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
                const int mx = event->pos().x();
                const bool onHandle = std::abs(mx - levelX(plot, m_minLevel)) <= 8
                    || std::abs(mx - levelX(plot, m_maxLevel)) <= 8;
                setCursor(onHandle ? Qt::SizeHorCursor : Qt::ArrowCursor);
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
                m_autoLevelsCallback();
                event->accept();
                return;
            }
            QWidget::mouseDoubleClickEvent(event);
        }

    private:
        enum class DragMode { None, MinLevel, MaxLevel };

        int levelX(const QRect& plot, int level) const
        {
            return plot.left() + static_cast<int>(static_cast<qint64>(level) * plot.width() / m_domainMax);
        }

        void updateLevelFromMouse(int mouseX)
        {
            const QRect plot = plotRect();
            if (plot.width() <= 0) return;
            const int rawVal = static_cast<int>(static_cast<qint64>(mouseX - plot.left()) * m_domainMax / plot.width());
            const int newMin = m_dragMode == DragMode::MinLevel ? qBound(0, rawVal, m_maxLevel - 1) : m_minLevel;
            const int newMax = m_dragMode == DragMode::MaxLevel ? qBound(m_minLevel + 1, rawVal, m_domainMax) : m_maxLevel;
            if (newMin != m_minLevel || newMax != m_maxLevel)
            {
                m_minLevel = newMin;
                m_maxLevel = newMax;
                m_levelsCallback(m_minLevel, m_maxLevel);
                update();
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
    {
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
    }

    // Builds the device control layout
    void DeviceControlWidget::setupUI()
    {
        m_imageControlsWidget = makeScrollArea(this);
        auto* imageContainer = new QWidget(m_imageControlsWidget);
        auto* imageLayout = new QVBoxLayout(imageContainer);
        imageLayout->setSpacing(5);
        imageLayout->setContentsMargins(5, 5, 5, 5);
        imageLayout->addWidget(createPreviewControlsGroup());
        imageLayout->addStretch();
        m_imageControlsWidget->setWidget(imageContainer);

        m_hardwareControlsWidget = makeScrollArea(this);
        auto* hardwareContainer = new QWidget(m_hardwareControlsWidget);
        auto* hardwareLayout = new QVBoxLayout(hardwareContainer);
        hardwareLayout->setSpacing(5);
        hardwareLayout->setContentsMargins(5, 5, 5, 5);
        hardwareLayout->addWidget(createControlGroup());
        hardwareLayout->addWidget(createStageGroup());
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

    // Connects the live preview, the one preview every image is shown in. It is called once,
    // because calling it again on each viewer change added every connection one more time.
    void DeviceControlWidget::setPreviewWidget(PreviewWidget* preview)
    {
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

        // Selected layer transform values follow the live cameras
        connect(m_previewWidget, &PreviewWidget::availableCameraIdsChanged,
                this, &DeviceControlWidget::refreshPreviewLayerSettings);
        connect(m_previewWidget, &PreviewWidget::availableLayerKeysChanged,
                this, &DeviceControlWidget::onPreviewAvailableLayerKeysChanged);
        connect(m_previewWidget, &PreviewWidget::visibleLayerKeysChanged,
                this, [this](const QStringList& layerKeys)
                {
                    applyPreviewVisibility(layerKeys, false);
                });
        connect(m_previewWidget, &PreviewWidget::layerInfoTextChanged,
                this, &DeviceControlWidget::refreshPreviewLayerInfoText);
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layerDisplayChanged,
                this, [this](const QString& layerKey)
                {
                    refreshLayerSwatch(layerKey);
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

        onPreviewAvailableLayerKeysChanged(m_previewWidget->availableLayerKeys());
        refreshPreviewLayerInfoText();

        {
            const QSignalBlocker clippingBlocker(m_clippingCheckBox);
            const QSignalBlocker scaleBarBlocker(m_scaleBarCheckBox);
            const QSignalBlocker dimensionBlocker(m_viewDimensionCombo);
            const QSignalBlocker sliderBlocker(m_3dZScaleSlider);
            const QSignalBlocker spinBlocker(m_3dZScaleSpinBox);
            const QSignalBlocker wireframeBlocker(m_3dWireframeCheckBox);
            const QSignalBlocker colorbarBlocker(m_3dColorbarCheckBox);
            m_clippingCheckBox->setChecked(m_previewWidget->isClippingWarningEnabled());
            m_scaleBarCheckBox->setChecked(m_previewWidget->isScaleBarVisible());
            m_viewDimensionCombo->setCurrentIndex(
                m_previewWidget->viewDimensionMode() == PreviewWidget::ViewDimensionMode::ThreeDimensional
                    ? 1
                    : 0);
            m_3dZScaleSlider->setValue(qRound(m_previewWidget->get3dZScale() * 10.0f));
            m_3dZScaleSpinBox->setValue(m_previewWidget->get3dZScale());
            m_3dWireframeCheckBox->setChecked(m_previewWidget->is3dWireframeEnabled());
            m_3dColorbarCheckBox->setChecked(m_previewWidget->isThreeDimensionalColorbarVisible());
        }
        connect(m_previewWidget, &PreviewWidget::clippingWarningChanged,
                this, [this](bool enabled)
                {
                    const QSignalBlocker blocker(m_clippingCheckBox);
                    m_clippingCheckBox->setChecked(enabled);
                });
        connect(m_previewWidget, &PreviewWidget::scaleBarVisibilityChanged,
                this, [this](bool visible)
                {
                    const QSignalBlocker blocker(m_scaleBarCheckBox);
                    m_scaleBarCheckBox->setChecked(visible);
                });
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
        connect(m_previewWidget, &PreviewWidget::threeDimensionalColorbarVisibilityChanged,
                this, [this](bool visible)
                {
                    const QSignalBlocker blocker(m_3dColorbarCheckBox);
                    m_3dColorbarCheckBox->setChecked(visible);
                });
    }

    // Builds layer and alignment controls
    QWidget* DeviceControlWidget::createPreviewControlsGroup()
    {
        auto* previewControlsGroup = new QGroupBox("Layers", this);
        QGridLayout* controlLayout = new QGridLayout(previewControlsGroup);
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
        m_layerTable->setEditTriggers(QAbstractItemView::DoubleClicked
                                      | QAbstractItemView::EditKeyPressed);
        m_layerTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_layerTable->setDragEnabled(true);
        m_layerTable->setAcceptDrops(true);
        m_layerTable->viewport()->setAcceptDrops(true);
        m_layerTable->setDragDropMode(QAbstractItemView::DragDrop);
        m_layerTable->setDropIndicatorShown(true);
        m_layerTable->viewport()->installEventFilter(this);
        m_layerTable->setShowGrid(false);
        m_layerTable->setMinimumHeight(94);
        m_layerTable->setMaximumHeight(150);

        auto* layerHistogramGroup = new QGroupBox(QStringLiteral("Histogram"), previewControlsGroup);
        auto* histogramLayout = new QVBoxLayout(layerHistogramGroup);
        histogramLayout->setContentsMargins(6, 6, 6, 6);
        m_layerHistogramWidget = new LayerHistogramWidget(layerHistogramGroup);
        histogramLayout->addWidget(m_layerHistogramWidget);
        auto* histogramLevelsLayout = new QHBoxLayout;
        m_layerAutoButton = new QPushButton(QStringLiteral("Auto"), layerHistogramGroup);
        m_layerAutoButton->setToolTip(QStringLiteral("Set display levels from the current image"));
        m_layerFullRangeButton = new QPushButton(QStringLiteral("Full Range"), layerHistogramGroup);
        m_layerFullRangeButton->setToolTip(QStringLiteral("Show the complete intensity range"));
        histogramLevelsLayout->addWidget(m_layerAutoButton);
        histogramLevelsLayout->addWidget(m_layerFullRangeButton);
        histogramLayout->addLayout(histogramLevelsLayout);

        m_layerAutoStretchCheckBox = new QCheckBox(QStringLiteral("Continuous Auto"), layerHistogramGroup);
        m_layerAutoStretchCheckBox->setToolTip(
            QStringLiteral("Update display levels continuously as images arrive"));

        auto* layerHistogramLogCheckBox = new QCheckBox(QStringLiteral("Log scale"), layerHistogramGroup);
        auto* histogramOptionsLayout = new QHBoxLayout();
        histogramOptionsLayout->addWidget(m_layerAutoStretchCheckBox);
        histogramOptionsLayout->addWidget(layerHistogramLogCheckBox);
        histogramOptionsLayout->addStretch(1);
        histogramLayout->addLayout(histogramOptionsLayout);
        connect(layerHistogramLogCheckBox, &QCheckBox::toggled,
                m_layerHistogramWidget, &LayerHistogramWidget::setLogScale);

        m_layerSettingsGroup = new QGroupBox("Layer Settings", this);
        QGridLayout* layerSettingsLayout = new QGridLayout(m_layerSettingsGroup);
        layerSettingsLayout->setContentsMargins(6, 6, 6, 6);
        layerSettingsLayout->setHorizontalSpacing(6);
        layerSettingsLayout->setVerticalSpacing(4);

        m_selectedLayerLabel = new QLabel(QStringLiteral("No layer selected"), m_layerSettingsGroup);
        m_selectedLayerLabel->setWordWrap(true);


        m_layerOpacitySpinBox = new QSpinBox(m_layerSettingsGroup);
        m_layerOpacitySpinBox->setRange(0, 100);
        m_layerOpacitySpinBox->setSuffix(QStringLiteral("%"));
        m_layerOpacitySpinBox->setKeyboardTracking(false);

        m_layerGammaSpinBox = new QDoubleSpinBox(m_layerSettingsGroup);
        m_layerGammaSpinBox->setRange(0.2, 2.0);
        m_layerGammaSpinBox->setDecimals(2);
        m_layerGammaSpinBox->setSingleStep(0.02);
        m_layerGammaSpinBox->setKeyboardTracking(false);

        m_layerColormapComboBox = new QComboBox(m_layerSettingsGroup);
        m_layerBlendingComboBox = new QComboBox(m_layerSettingsGroup);
        m_clippingCheckBox = new QCheckBox(QStringLiteral("Hi-Lo Warn"), m_layerSettingsGroup);
        m_clippingCheckBox->setToolTip(QStringLiteral("Highlight saturated pixels in red and zero pixels in blue (Hotkey: C)"));
        m_scaleBarCheckBox = new QCheckBox(QStringLiteral("Scale Bar"), m_layerSettingsGroup);
        m_scaleBarCheckBox->setToolTip(QStringLiteral("Display calibrated scale bar in viewport"));
        m_scaleBarCheckBox->setChecked(true);

        layerSettingsLayout->addWidget(m_selectedLayerLabel, 0, 0, 1, 6);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Opacity:"), m_layerSettingsGroup), 1, 0);
        layerSettingsLayout->addWidget(m_layerOpacitySpinBox, 1, 1, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Gamma:"), m_layerSettingsGroup), 1, 2);
        layerSettingsLayout->addWidget(m_layerGammaSpinBox, 1, 3, Qt::AlignLeft);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Color:"), m_layerSettingsGroup), 2, 0);
        layerSettingsLayout->addWidget(m_layerColormapComboBox, 2, 1);
        layerSettingsLayout->addWidget(new QLabel(QStringLiteral("Blend:"), m_layerSettingsGroup), 2, 2);
        layerSettingsLayout->addWidget(m_layerBlendingComboBox, 2, 3, 1, 3);
        layerSettingsLayout->addWidget(m_clippingCheckBox, 3, 0, 1, 3);
        layerSettingsLayout->addWidget(m_scaleBarCheckBox, 3, 3, 1, 3);

        auto* transformGroup = new QGroupBox(QStringLiteral("Transform"), this);
        auto* transformLayout = new QGridLayout(transformGroup);
        transformLayout->setContentsMargins(6, 6, 6, 6);
        auto* alignXLabel = new QLabel("X offset:", transformGroup);
        m_alignXSpinBox = new QSpinBox(transformGroup);
        m_alignXSpinBox->setRange(-1000, 1000);
        m_alignXSpinBox->setValue(0);
        m_alignXSpinBox->setKeyboardTracking(false);

        auto* alignYLabel = new QLabel("Y offset:", transformGroup);
        m_alignYSpinBox = new QSpinBox(transformGroup);
        m_alignYSpinBox->setRange(-1000, 1000);
        m_alignYSpinBox->setValue(0);
        m_alignYSpinBox->setKeyboardTracking(false);

        auto* alignZoomLabel = new QLabel("Scale:", transformGroup);
        m_alignZoomSpinBox = new QSpinBox(transformGroup);
        m_alignZoomSpinBox->setRange(10, 500);
        m_alignZoomSpinBox->setValue(100);
        m_alignZoomSpinBox->setToolTip("Source camera display scale percent");
        m_alignZoomSpinBox->setKeyboardTracking(false);

        m_alignFlipXCheckBox = new QCheckBox("Flip X", transformGroup);
        m_alignFlipYCheckBox = new QCheckBox("Flip Y", transformGroup);
        auto* alignResetButton = new QPushButton("Reset", transformGroup);
        fitButtonToText(alignResetButton);
        alignResetButton->setToolTip("Reset offset and flip");

        transformLayout->addWidget(alignXLabel, 0, 0);
        transformLayout->addWidget(m_alignXSpinBox, 0, 1, Qt::AlignLeft);
        transformLayout->addWidget(alignYLabel, 0, 2);
        transformLayout->addWidget(m_alignYSpinBox, 0, 3, Qt::AlignLeft);
        transformLayout->addWidget(alignZoomLabel, 1, 0);
        transformLayout->addWidget(m_alignZoomSpinBox, 1, 1, Qt::AlignLeft);
        transformLayout->addWidget(m_alignFlipXCheckBox, 1, 2, Qt::AlignLeft);
        transformLayout->addWidget(m_alignFlipYCheckBox, 1, 3, Qt::AlignLeft);
        transformLayout->addWidget(alignResetButton, 1, 4, Qt::AlignLeft);
        transformLayout->setColumnStretch(5, 1);

        alignXLabel->setMinimumWidth(20);
        alignYLabel->setMinimumWidth(20);
        alignZoomLabel->setMinimumWidth(60);

        auto* surfaceViewGroup = new QGroupBox(QStringLiteral("Surface View"), this);
        auto* surfaceViewLayout = new QGridLayout(surfaceViewGroup);
        surfaceViewLayout->setContentsMargins(6, 6, 6, 6);
        surfaceViewLayout->setHorizontalSpacing(6);
        surfaceViewLayout->setVerticalSpacing(4);

        m_viewDimensionCombo = new QComboBox(surfaceViewGroup);
        m_viewDimensionCombo->addItem(QStringLiteral("2D Flat Map"));
        m_viewDimensionCombo->addItem(QStringLiteral("3D Surface"));
        m_viewDimensionCombo->setToolTip(QStringLiteral("Show the selected layer as a flat image or as a displaced surface. Other layers keep their own."));

        m_3dZScaleSlider = new QSlider(Qt::Horizontal, surfaceViewGroup);
        m_3dZScaleSlider->setRange(1, 100);
        m_3dZScaleSlider->setValue(10);
        m_3dZScaleSlider->setToolTip(QStringLiteral("Height exaggeration from 0.1x to 10.0x"));

        m_3dZScaleSpinBox = new QDoubleSpinBox(surfaceViewGroup);
        // Down to a hundredth, because a small feature on a wide range of values stands up like a spike at 1x.
        // The step follows the size of the value, so that it is as easy to go from 0.05 to 0.04 as from 5 to 4.
        m_3dZScaleSpinBox->setRange(0.01, 10.0);
        m_3dZScaleSpinBox->setDecimals(2);
        m_3dZScaleSpinBox->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        m_3dZScaleSpinBox->setSuffix(QStringLiteral("x"));
        m_3dZScaleSpinBox->setValue(1.0);
        m_3dZScaleSpinBox->setFixedWidth(64);
        m_3dZScaleSpinBox->setKeyboardTracking(false);

        m_3dWireframeCheckBox = new QCheckBox(QStringLiteral("Wireframe"), surfaceViewGroup);
        m_3dColorbarCheckBox = new QCheckBox(QStringLiteral("Colorbar"), surfaceViewGroup);
        m_3dColorbarCheckBox->setChecked(true);
        auto* reset3dButton = new QPushButton(QStringLiteral("Reset View"), surfaceViewGroup);
        reset3dButton->setMaximumWidth(84);

        surfaceViewLayout->addWidget(new QLabel(QStringLiteral("Mode:"), surfaceViewGroup), 0, 0);
        surfaceViewLayout->addWidget(m_viewDimensionCombo, 0, 1, 1, 3);
        surfaceViewLayout->addWidget(new QLabel(QStringLiteral("Z-Scale:"), surfaceViewGroup), 1, 0);
        surfaceViewLayout->addWidget(m_3dZScaleSlider, 1, 1);
        surfaceViewLayout->addWidget(m_3dZScaleSpinBox, 1, 2);
        surfaceViewLayout->addWidget(m_3dWireframeCheckBox, 2, 0, 1, 2);
        surfaceViewLayout->addWidget(m_3dColorbarCheckBox, 2, 2);
        surfaceViewLayout->addWidget(reset3dButton, 2, 3, 1, 1, Qt::AlignLeft);

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
        connect(m_3dColorbarCheckBox, &QCheckBox::toggled,
                this, [this](bool visible) { m_previewWidget->setThreeDimensionalColorbarVisible(visible); });
        connect(m_clippingCheckBox, &QCheckBox::toggled,
                this, [this](bool enabled) { m_previewWidget->setClippingWarningEnabled(enabled); });
        connect(m_scaleBarCheckBox, &QCheckBox::toggled,
                this, [this](bool visible) { m_previewWidget->setScaleBarVisible(visible); });
        connect(reset3dButton, &QPushButton::clicked,
                this, [this]() { m_previewWidget->reset3dCamera(); });

        controlLayout->addWidget(m_layerTable, 0, 0, 1, 6);
        controlLayout->addWidget(layerHistogramGroup, 1, 0, 1, 6);
        controlLayout->addWidget(m_layerSettingsGroup, 2, 0, 1, 6);
        controlLayout->addWidget(transformGroup, 3, 0, 1, 6);
        controlLayout->addWidget(surfaceViewGroup, 4, 0, 1, 6);

        controlLayout->setColumnStretch(5, 1);

        connect(m_layerTable, &QTableWidget::currentCellChanged,
                this, &DeviceControlWidget::onPreviewLayerSelectionChanged);
        connect(m_layerTable, &QTableWidget::itemChanged,
                this, &DeviceControlWidget::onPreviewLayerTableItemChanged);
        connect(m_layerTable, &QTableWidget::customContextMenuRequested,
                this, &DeviceControlWidget::showLayerContextMenu);
        auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), m_layerTable);
        deleteShortcut->setContext(Qt::WidgetShortcut);
        connect(deleteShortcut, &QShortcut::activated, this, [this]() { removeSelectedLayer(); });
        auto* duplicateShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), m_layerTable);
        duplicateShortcut->setContext(Qt::WidgetShortcut);
        connect(duplicateShortcut, &QShortcut::activated, this, [this]() { onPreviewLayerDuplicateClicked(); });
        auto* moveUpShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Up")), m_layerTable);
        moveUpShortcut->setContext(Qt::WidgetShortcut);
        connect(moveUpShortcut, &QShortcut::activated, this, [this]() { m_sceneModel->moveLayer(currentLayerKey(), -1); });
        auto* moveDownShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Down")), m_layerTable);
        moveDownShortcut->setContext(Qt::WidgetShortcut);
        connect(moveDownShortcut, &QShortcut::activated, this, [this]() { m_sceneModel->moveLayer(currentLayerKey(), 1); });
        connect(m_layerOpacitySpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerOpacityChanged);
        connect(m_layerGammaSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &DeviceControlWidget::onPreviewLayerGammaChanged);
        connect(m_layerColormapComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerColormapChanged);
        connect(m_layerBlendingComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceControlWidget::onPreviewLayerBlendingChanged);
        connect(m_layerAutoButton, &QPushButton::clicked,
                this, [this]() { m_workspace->autoLayerLevels(currentLayerKey()); });
        connect(m_layerFullRangeButton, &QPushButton::clicked,
                this, [this]() { m_workspace->fullLayerLevels(currentLayerKey()); });
        connect(m_layerAutoStretchCheckBox, &QCheckBox::toggled,
                this, &DeviceControlWidget::onPreviewLayerAutoStretchToggled);
        m_layerHistogramWidget->setOnLevelsChanged([this](int minLevel, int maxLevel)
        {
            const QString layerKey = currentLayerKey();
            scopeone::core::DocumentLayer layer;
            if (m_sceneModel->findLayer(layerKey, layer))
            {
                m_workspace->setLayerAutoStretchEnabled(layerKey, false);
                m_layerAutoStretchCheckBox->setChecked(false);
                m_sceneModel->setLayerDisplayLevels(layerKey, minLevel, maxLevel, layer.display.levelDomainMax);
            }
        });
        m_layerHistogramWidget->setOnAutoLevelsRequested([this]()
        {
            m_workspace->autoLayerLevels(currentLayerKey());
        });
        // The scene model ignores a transform for a source it does not hold, such as no selection
        const auto applySourceTransform = [this]()
        {
            scopeone::core::ImageSceneModel::SourceDisplayTransform transform;
            transform.offsetX = m_alignXSpinBox->value();
            transform.offsetY = m_alignYSpinBox->value();
            transform.zoomPercent = m_alignZoomSpinBox->value();
            transform.flipX = m_alignFlipXCheckBox->isChecked();
            transform.flipY = m_alignFlipYCheckBox->isChecked();
            m_sceneModel->setSourceDisplayTransform(selectedLayerSourceId(), transform);
        };
        for (QSpinBox* spinBox : {m_alignXSpinBox, m_alignYSpinBox, m_alignZoomSpinBox})
        {
            connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, applySourceTransform);
        }
        for (QCheckBox* checkBox : {m_alignFlipXCheckBox, m_alignFlipYCheckBox})
        {
            connect(checkBox, &QCheckBox::toggled, this, applySourceTransform);
        }
        // The editors follow through sourceDisplayTransformChanged
        connect(alignResetButton, &QPushButton::clicked,
                this, [this]() { m_sceneModel->resetSourceDisplayTransform(selectedLayerSourceId()); });

        refreshPreviewLayerSettings();
        return previewControlsGroup;
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
            visibleCheckBox->installEventFilter(this);
            m_layerTable->setCellWidget(row, 0, visibleCheckBox);

            QTableWidgetItem* nameItem = new QTableWidgetItem(m_previewWidget->layerName(layerKey));
            nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
            nameItem->setData(Qt::UserRole, layerKey);
            m_layerTable->setItem(row, 1, nameItem);

            QTableWidgetItem* infoItem = new QTableWidgetItem(m_previewWidget->layerInfoText(layerKey));
            infoItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            infoItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_layerTable->setItem(row, 2, infoItem);

            m_layerRows.insert(layerKey, visibleCheckBox);
            refreshLayerSwatch(layerKey);
            m_layerTable->setRowHeight(row, 26);
            ++row;
        };

        for (const QString& layerKey : layerKeys)
        {
            addLayerRow(layerKey);
        }

        int selectedRow = layerKeys.indexOf(previousLayerKey);
        if (selectedRow < 0)
        {
            const QStringList visibleLayerKeys = m_previewWidget->visibleLayerKeys();
            for (const QString& visibleLayerKey : visibleLayerKeys)
            {
                selectedRow = layerKeys.indexOf(visibleLayerKey);
                if (selectedRow >= 0)
                {
                    break;
                }
            }
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
            m_layerTable->item(row, 2)->setText(m_previewWidget->layerInfoText(layerKeyAt(row)));
        }
    }

    QString DeviceControlWidget::layerKeyAt(int row) const
    {
        const QTableWidgetItem* item = m_layerTable->item(row, 1);
        return item ? item->data(Qt::UserRole).toString() : QString();
    }

    int DeviceControlWidget::layerRow(const QString& layerKey) const
    {
        for (int row = 0; row < m_layerTable->rowCount(); ++row)
        {
            if (layerKeyAt(row) == layerKey)
            {
                return row;
            }
        }
        return -1;
    }

    // Applies layer visibility to layer rows and preview
    void DeviceControlWidget::applyPreviewVisibility(const QStringList& layerKeys, bool notifyPreview)
    {
        const QSet<QString> visibleLayerKeySet(layerKeys.begin(), layerKeys.end());

        for (auto it = m_layerRows.begin(); it != m_layerRows.end(); ++it)
        {
            QSignalBlocker blocker(it.value());
            it.value()->setChecked(visibleLayerKeySet.contains(it.key()));
        }

        if (notifyPreview)
        {
            m_sceneModel->setVisibleLayers(layerKeys);
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
        const scopeone::core::ImageSceneModel::SourceDisplayTransform transform =
            m_sceneModel->sourceDisplayTransform(selectedLayerSourceId());

        const QSignalBlocker opacityBlocker(m_layerOpacitySpinBox);
        const QSignalBlocker gammaBlocker(m_layerGammaSpinBox);
        const QSignalBlocker colormapBlocker(m_layerColormapComboBox);
        const QSignalBlocker blendingBlocker(m_layerBlendingComboBox);
        const QSignalBlocker autoStretchBlocker(m_layerAutoStretchCheckBox);
        const QSignalBlocker xBlocker(m_alignXSpinBox);
        const QSignalBlocker yBlocker(m_alignYSpinBox);
        const QSignalBlocker zoomBlocker(m_alignZoomSpinBox);
        const QSignalBlocker flipXBlocker(m_alignFlipXCheckBox);
        const QSignalBlocker flipYBlocker(m_alignFlipYCheckBox);
        m_layerOpacitySpinBox->setValue(layer.display.opacityPercent);
        m_layerGammaSpinBox->setValue(layer.display.gamma);
        m_layerColormapComboBox->setCurrentIndex(m_layerColormapComboBox->findText(layer.display.colormap));
        m_layerBlendingComboBox->setCurrentIndex(m_layerBlendingComboBox->findText(layer.display.blending));
        m_layerAutoStretchCheckBox->setChecked(m_workspace->layerAutoStretchEnabled(layerKey));
        m_layerOpacitySpinBox->setEnabled(layer.display.blending != QStringLiteral("Opaque"));
        m_layerHistogramWidget->setLevels(
            layer.display.levelMin, layer.display.levelMax, layer.display.levelDomainMax);
        m_alignXSpinBox->setValue(transform.offsetX);
        m_alignYSpinBox->setValue(transform.offsetY);
        m_alignZoomSpinBox->setValue(transform.zoomPercent);
        m_alignFlipXCheckBox->setChecked(transform.flipX);
        m_alignFlipYCheckBox->setChecked(transform.flipY);
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
        const int row = layerRow(currentLayerKey());
        {
            const QSignalBlocker blocker(m_layerTable);
            if (row >= 0)
            {
                m_layerTable->setCurrentCell(row, 1);
            }
            else
            {
                m_layerTable->clearSelection();
            }
        }
        refreshPreviewLayerSettings();
    }

    void DeviceControlWidget::onPreviewAvailableLayerKeysChanged(const QStringList& layerKeys)
    {
        rebuildPreviewLayerTable(layerKeys);
        applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
        refreshLayerHistogram();
        updateControlsState();
    }

    // The check box only shows or hides the layer. The selected layer, which the blue row marks and the
    // settings below belong to, changes only when a row is clicked. Tying the two together made the
    // selection jump when a layer was hidden or shown.
    void DeviceControlWidget::onPreviewLayerVisibleToggled(bool checked)
    {
        const QString layerKey = sender()->property("layerKey").toString();
        m_sceneModel->setLayerVisible(layerKey, checked);
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
    }

    void DeviceControlWidget::onPreviewLayerAutoStretchToggled(bool enabled)
    {
        m_workspace->setLayerAutoStretchEnabled(currentLayerKey(), enabled);
    }

    void DeviceControlWidget::onPreviewLayerSelectionChanged(int currentRow, int, int, int)
    {
        QTableWidgetItem* item = m_layerTable->item(currentRow, 1);
        const QString layerKey = item ? item->data(Qt::UserRole).toString() : QString();
        m_workspace->setActiveLayerKey(layerKey);
        refreshPreviewLayerSettings();
        refreshLayerHistogram();
        updateControlsState();
    }

    void DeviceControlWidget::onPreviewLayerDuplicateClicked()
    {
        const QString sourceLayerKey = currentLayerKey();
        const scopeone::core::ImageFrame sourceFrame = m_scopeonecore->graphFrame(sourceLayerKey);
        scopeone::core::DocumentLayer sourceLayer;
        m_sceneModel->findLayer(sourceLayerKey, sourceLayer);

        const QString sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(sourceLayerKey);
        int copyIndex = 1;
        QString duplicateSourceId;
        do
        {
            duplicateSourceId = QStringLiteral("copy:%1_%2").arg(sourceId).arg(copyIndex++);
        }
        while (m_sceneModel->hasSource(duplicateSourceId));

        scopeone::core::ImageFrame duplicateFrame = sourceFrame;
        duplicateFrame.cameraId = duplicateSourceId;
        const QString duplicateName = QStringLiteral("%1 (Copy)")
                                          .arg(sourceLayer.name.isEmpty()
                                                   ? m_previewWidget->layerName(sourceLayerKey)
                                                   : sourceLayer.name);
        const scopeone::core::ImageFrame published = m_scopeonecore->publishStaticFrame(
            duplicateSourceId, duplicateFrame, duplicateName);
        const QString duplicateLayerKey =
            scopeone::core::ScopeOneCore::staticLayerKey(published.cameraId);

        m_sceneModel->setLayerVisible(duplicateLayerKey, sourceLayer.display.visible);
        m_sceneModel->setLayerOpacityPercent(
            duplicateLayerKey, sourceLayer.display.opacityPercent);
        m_sceneModel->setLayerGamma(duplicateLayerKey, sourceLayer.display.gamma);
        m_sceneModel->setLayerColormap(duplicateLayerKey, sourceLayer.display.colormap);
        m_sceneModel->setLayerBlending(duplicateLayerKey, sourceLayer.display.blending);
        m_sceneModel->setLayerDisplayLevels(duplicateLayerKey,
                                            sourceLayer.display.levelMin,
                                            sourceLayer.display.levelMax,
                                            sourceLayer.display.levelDomainMax);
        m_workspace->setActiveLayerKey(duplicateLayerKey);
    }

    void DeviceControlWidget::onPreviewLayerTableItemChanged(QTableWidgetItem* item)
    {
        if (item->column() == 1)
        {
            m_sceneModel->setLayerName(item->data(Qt::UserRole).toString(), item->text());
        }
    }

    void DeviceControlWidget::showLayerContextMenu(const QPoint& pos)
    {
        const int row = m_layerTable->rowAt(pos.y());
        if (row < 0)
        {
            return;
        }

        m_layerTable->setCurrentCell(row, 1);
        const QString layerKey = currentLayerKey();
        QMenu menu(this);
        QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
        QAction* duplicateAction = menu.addAction(QStringLiteral("Duplicate Layer"));
        QAction* removeAction = menu.addAction(QStringLiteral("Remove Layer"));
        removeAction->setEnabled(canRemoveSelectedLayer());
        menu.addSeparator();
        QAction* soloAction = menu.addAction(QStringLiteral("Show Only This Layer"));
        QAction* showAllAction = menu.addAction(QStringLiteral("Show All Layers"));
        menu.addSeparator();
        QAction* autoLevelsAction = menu.addAction(QStringLiteral("Auto Levels"));
        QAction* copyLevelsAction = menu.addAction(QStringLiteral("Copy Levels to All Layers"));
        QAction* selectedAction = menu.exec(m_layerTable->viewport()->mapToGlobal(pos));
        if (!selectedAction)
        {
            return;
        }
        if (selectedAction == renameAction)
        {
            m_layerTable->editItem(m_layerTable->item(row, 1));
        }
        else if (selectedAction == duplicateAction)
        {
            onPreviewLayerDuplicateClicked();
        }
        else if (selectedAction == removeAction)
        {
            removeSelectedLayer();
        }
        else if (selectedAction == soloAction)
        {
            m_sceneModel->setVisibleLayers({layerKey});
            applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
        }
        else if (selectedAction == showAllAction)
        {
            m_sceneModel->setVisibleLayers(m_layerRows.keys());
            applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
        }
        else if (selectedAction == autoLevelsAction)
        {
            m_workspace->autoLayerLevels(layerKey);
        }
        else if (selectedAction == copyLevelsAction)
        {
            scopeone::core::DocumentLayer source;
            if (m_sceneModel->findLayer(layerKey, source))
            {
                for (const QString& key : m_layerRows.keys())
                {
                    m_sceneModel->setLayerDisplayLevels(key, source.display.levelMin, source.display.levelMax,
                                                        source.display.levelDomainMax);
                }
            }
        }
    }

    // Only images that were opened or made can be removed, not the live cameras
    bool DeviceControlWidget::canRemoveSelectedLayer() const
    {
        return scopeone::core::ScopeOneCore::isStaticLayerKey(currentLayerKey());
    }

    // Takes the selected layer away after the user agrees
    void DeviceControlWidget::removeSelectedLayer()
    {
        if (!canRemoveSelectedLayer())
        {
            return;
        }
        const QString layerKey = currentLayerKey();
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Remove Layer"),
            QStringLiteral("Remove the layer \"%1\"?").arg(m_previewWidget->layerName(layerKey)));
        if (answer == QMessageBox::Yes)
        {
            m_scopeonecore->removeStaticFrame(scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey));
        }
    }

    // Shows one layer alone, or all of them when it already is the only one
    void DeviceControlWidget::showOnlyLayer(const QString& layerKey)
    {
        QStringList shown;
        for (auto it = m_layerRows.constBegin(); it != m_layerRows.constEnd(); ++it)
        {
            if (it.value()->isChecked())
            {
                shown.append(it.key());
            }
        }
        const bool alone = shown.size() == 1 && shown.first() == layerKey;
        m_sceneModel->setVisibleLayers(alone ? m_layerRows.keys() : QStringList{layerKey});
        applyPreviewVisibility(m_previewWidget->visibleLayerKeys(), false);
    }

    // Shows the color of the layer in front of its name
    void DeviceControlWidget::refreshLayerSwatch(const QString& layerKey)
    {
        scopeone::core::DocumentLayer layer;
        if (!m_sceneModel->findLayer(layerKey, layer))
        {
            return;
        }
        static const QHash<QString, QList<QColor>> stops{
            {QStringLiteral("Gray"), {QColor(40, 40, 40), QColor(235, 235, 235)}},
            {QStringLiteral("Inverted Gray"), {QColor(235, 235, 235), QColor(40, 40, 40)}},
            {QStringLiteral("Green"), {QColor(0, 40, 0), QColor(0, 255, 0)}},
            {QStringLiteral("Magenta"), {QColor(40, 0, 40), QColor(255, 0, 255)}},
            {QStringLiteral("Cyan"), {QColor(0, 40, 40), QColor(0, 255, 255)}},
            {QStringLiteral("Red"), {QColor(40, 0, 0), QColor(255, 0, 0)}},
            {QStringLiteral("Blue"), {QColor(0, 0, 40), QColor(0, 80, 255)}},
            {QStringLiteral("Yellow"), {QColor(40, 40, 0), QColor(255, 255, 0)}},
            {QStringLiteral("Fire"), {QColor(0, 0, 0), QColor(220, 40, 0), QColor(255, 240, 120)}},
            {QStringLiteral("Ice"), {QColor(0, 0, 60), QColor(0, 140, 255), QColor(220, 255, 255)}},
            {QStringLiteral("Viridis"), {QColor(68, 1, 84), QColor(33, 145, 140), QColor(253, 231, 37)}},
            {QStringLiteral("Inferno"), {QColor(0, 0, 4), QColor(187, 55, 84), QColor(252, 255, 164)}},
            {QStringLiteral("Magma"), {QColor(0, 0, 4), QColor(183, 55, 121), QColor(252, 253, 191)}},
            {QStringLiteral("Cividis"), {QColor(0, 32, 77), QColor(124, 123, 120), QColor(253, 231, 55)}},
            {QStringLiteral("Jet"), {QColor(0, 0, 160), QColor(0, 255, 0), QColor(160, 0, 0)}},
            {QStringLiteral("Rainbow"), {QColor(130, 0, 200), QColor(0, 200, 0), QColor(255, 0, 0)}},
            {QStringLiteral("Turbo"), {QColor(48, 18, 59), QColor(164, 252, 60), QColor(122, 4, 3)}},
        };
        QList<QColor> colors = stops.value(layer.display.colormap);
        if (colors.isEmpty())
        {
            colors = {QColor(40, 40, 40), QColor(235, 235, 235)};
        }
        QPixmap swatch(18, 10);
        QLinearGradient gradient(0, 0, 18, 0);
        for (int i = 0; i < colors.size(); ++i)
        {
            gradient.setColorAt(colors.size() == 1 ? 0.0 : static_cast<double>(i) / (colors.size() - 1), colors.at(i));
        }
        QPainter painter(&swatch);
        painter.fillRect(swatch.rect(), gradient);
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
        painter.end();
        const int row = layerRow(layerKey);
        if (row >= 0)
        {
            const QSignalBlocker blocker(m_layerTable);
            m_layerTable->item(row, 1)->setIcon(QIcon(swatch));
        }
    }

    // Alt click on a check box shows one layer alone, and a drop in the list moves the layer
    bool DeviceControlWidget::eventFilter(QObject* watched, QEvent* event)
    {
        if (auto* box = qobject_cast<QCheckBox*>(watched))
        {
            const QEvent::Type type = event->type();
            if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
                || type == QEvent::MouseButtonDblClick)
            {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->modifiers() & Qt::AltModifier)
                {
                    if (type == QEvent::MouseButtonPress)
                    {
                        showOnlyLayer(box->property("layerKey").toString());
                    }
                    return true;
                }
                // A click anywhere in the cell switches the layer, not only on the small box
                if (mouse->button() == Qt::LeftButton)
                {
                    if (type != QEvent::MouseButtonRelease)
                    {
                        box->toggle();
                    }
                    return true;
                }
            }
        }
        else if (watched == m_layerTable->viewport())
        {
            const QEvent::Type type = event->type();
            if (type == QEvent::DragEnter || type == QEvent::DragMove)
            {
                auto* drag = static_cast<QDragMoveEvent*>(event);
                if (drag->source() == m_layerTable)
                {
                    drag->setDropAction(Qt::MoveAction);
                    drag->accept();
                    return true;
                }
            }
            else if (type == QEvent::Drop)
            {
                auto* drop = static_cast<QDropEvent*>(event);
                if (drop->source() == m_layerTable)
                {
                    const int from = m_layerTable->currentRow();
                    int to = m_layerTable->rowAt(drop->position().toPoint().y());
                    if (to < 0)
                    {
                        to = m_layerTable->rowCount() - 1;
                    }
                    if (from >= 0 && to != from)
                    {
                        m_sceneModel->moveLayer(currentLayerKey(), to - from);
                    }
                    drop->setDropAction(Qt::IgnoreAction);
                    drop->accept();
                    return true;
                }
            }
        }
        return QWidget::eventFilter(watched, event);
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

        scopeone::core::ScopeOneCore::HistogramStats stats;
        if (m_workspace->histogram(layerKey, stats))
        {
            m_layerHistogramWidget->setStats(stats);
        }
        else
        {
            m_layerHistogramWidget->clear();
        }
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

        layout->addWidget(new QLabel("Exposure (ms):", group), row, 0);
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
        connect(m_previewToggleButton, &QPushButton::clicked, this, [this]()
        {
            if (m_previewRunning)
            {
                emit stopPreviewRequested();
            }
            else
            {
                emit startPreviewRequested();
            }
        });
        m_snapButton = new QPushButton("Snap", group);
        m_snapButton->setMinimumHeight(30);
        m_snapButton->setToolTip(tr("Capture the latest frame from the selected target"));
        connect(m_snapButton, &QPushButton::clicked, this, [this]()
        {
            emit snapRequested(m_currentTarget);
        });
        QHBoxLayout* previewLayout = new QHBoxLayout();
        previewLayout->addWidget(m_previewToggleButton, 1);
        previewLayout->addWidget(m_snapButton, 1);
        layout->addLayout(previewLayout, row, 0, 1, 2);

        QHBoxLayout* roiLayout = new QHBoxLayout();
        m_drawROIButton = new QPushButton("Draw ROI", group);
        m_halfROIButton = new QPushButton("Half ROI", group);
        m_clearROIButton = new QPushButton("Restore ROI", group);
        roiLayout->addWidget(m_drawROIButton);
        roiLayout->addWidget(m_halfROIButton);
        roiLayout->addWidget(m_clearROIButton);
        layout->addLayout(roiLayout, ++row, 0, 1, 2);
        // The ROI buttons are enabled only while roiCameraTarget names a camera
        connect(m_drawROIButton, &QPushButton::clicked, this, [this]() { emit requestDrawROI(roiCameraTarget()); });
        connect(m_halfROIButton, &QPushButton::clicked, this, [this]() { emit requestHalfROI(roiCameraTarget()); });
        connect(m_clearROIButton, &QPushButton::clicked, this, [this]() { emit requestClearROI(roiCameraTarget()); });

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
        xyStepLayout->addWidget(new QLabel(QStringLiteral("Step (µm):")), 0, 0);
        m_xyStepLineEdit = makeStepLineEdit(QStringLiteral("10.0"));
        xyStepLayout->addWidget(m_xyStepLineEdit, 0, 1);

        xyStepLayout->addWidget(new QLabel(QStringLiteral("Big step (µm):")), 1, 0);
        m_xyBigStepLineEdit = makeStepLineEdit(QStringLiteral("100.0"));
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
        m_xyUpButton->setToolTip(QStringLiteral("Move +Y by step (Up arrow in the image view)"));
        m_xyDownButton->setToolTip(QStringLiteral("Move -Y by step (Down arrow in the image view)"));
        m_xyLeftButton->setToolTip(QStringLiteral("Move -X by step (Left arrow in the image view)"));
        m_xyRightButton->setToolTip(QStringLiteral("Move +X by step (Right arrow in the image view)"));
        m_xyBigUpButton->setToolTip(QStringLiteral("Move +Y by big step (Shift+Up arrow in the image view)"));
        m_xyBigDownButton->setToolTip(QStringLiteral("Move -Y by big step (Shift+Down arrow in the image view)"));
        m_xyBigLeftButton->setToolTip(QStringLiteral("Move -X by big step (Shift+Left arrow in the image view)"));
        m_xyBigRightButton->setToolTip(QStringLiteral("Move +X by big step (Shift+Right arrow in the image view)"));
        // Holding a step button keeps stepping. The moves are queued, so a slow stage only falls behind.
        for (QPushButton* button : {m_xyUpButton, m_xyDownButton, m_xyLeftButton, m_xyRightButton})
        {
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(500);
            button->setAutoRepeatInterval(250);
        }
        // Wide enough for the largest expected coordinate so positions are never clipped
        const int positionLabelWidth = fontMetrics().horizontalAdvance(QStringLiteral("X: -000000.00")) + 8;
        QWidget* xyCenterWidget = new QWidget();
        QVBoxLayout* xyCenterLayout = new QVBoxLayout(xyCenterWidget);
        xyCenterLayout->setContentsMargins(0, 0, 0, 0);
        xyCenterLayout->setSpacing(2);
        m_xPosLabel = new QLabel("X: N/A");
        m_yPosLabel = new QLabel("Y: N/A");
        m_xPosLabel->setMinimumWidth(positionLabelWidth);
        m_yPosLabel->setMinimumWidth(positionLabelWidth);
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
        zStepLayout->addWidget(new QLabel(QStringLiteral("Step (µm):")), 0, 0);
        m_zStepLineEdit = makeStepLineEdit(QStringLiteral("1.0"));
        zStepLayout->addWidget(m_zStepLineEdit, 0, 1);

        zStepLayout->addWidget(new QLabel(QStringLiteral("Big step (µm):")), 1, 0);
        m_zBigStepLineEdit = makeStepLineEdit(QStringLiteral("10.0"));
        zStepLayout->addWidget(m_zBigStepLineEdit, 1, 1);
        zColumn->addLayout(zStepLayout);

        QHBoxLayout* zControlLayout = new QHBoxLayout();
        QVBoxLayout* zButtonsLayout = new QVBoxLayout();
        m_zUpButton = new QPushButton(QStringLiteral("↑"));
        m_zDownButton = new QPushButton(QStringLiteral("↓"));
        m_zBigUpButton = new QPushButton(QStringLiteral("↑↑"));
        m_zBigDownButton = new QPushButton(QStringLiteral("↓↓"));
        m_zUpButton->setToolTip(QStringLiteral("Move +Z by step (Page Up in the image view)"));
        m_zDownButton->setToolTip(QStringLiteral("Move -Z by step (Page Down in the image view)"));
        m_zBigUpButton->setToolTip(QStringLiteral("Move +Z by big step (Shift+Page Up in the image view)"));
        m_zBigDownButton->setToolTip(QStringLiteral("Move -Z by big step (Shift+Page Down in the image view)"));
        for (QPushButton* button : {m_zUpButton, m_zDownButton})
        {
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(500);
            button->setAutoRepeatInterval(250);
        }
        zButtonsLayout->addWidget(m_zBigUpButton);
        zButtonsLayout->addWidget(m_zUpButton);
        m_zPosLabel = new QLabel("Z: N/A");
        m_zPosLabel->setMinimumWidth(positionLabelWidth);
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

        for (QComboBox* combo : {m_xyStageCombo, m_zStageCombo})
        {
            connect(combo, &QComboBox::currentTextChanged, this, [this]()
            {
                updateStageControlsEnabled();
                updateStagePositions();
            });
        }
        const struct
        {
            QPushButton* button;
            double dx;
            double dy;
            bool big;
        } xyMoves[] = {
            {m_xyUpButton, 0.0, 1.0, false},
            {m_xyDownButton, 0.0, -1.0, false},
            {m_xyLeftButton, -1.0, 0.0, false},
            {m_xyRightButton, 1.0, 0.0, false},
            {m_xyBigUpButton, 0.0, 1.0, true},
            {m_xyBigDownButton, 0.0, -1.0, true},
            {m_xyBigLeftButton, -1.0, 0.0, true},
            {m_xyBigRightButton, 1.0, 0.0, true},
        };
        for (const auto& move : xyMoves)
        {
            move.button->setFixedSize(28, 28);
            connect(move.button, &QPushButton::clicked, this, [this, move]() { moveXYStep(move.dx, move.dy, move.big); });
        }
        const struct
        {
            QPushButton* button;
            double dz;
            bool big;
        } zMoves[] = {
            {m_zUpButton, 1.0, false},
            {m_zDownButton, -1.0, false},
            {m_zBigUpButton, 1.0, true},
            {m_zBigDownButton, -1.0, true},
        };
        for (const auto& move : zMoves)
        {
            move.button->setFixedSize(28, 28);
            connect(move.button, &QPushButton::clicked, this, [this, move]() { moveZStep(move.dz, move.big); });
        }

        return group;
    }

    // Refreshes available XY and Z stage devices
    void DeviceControlWidget::refreshStageDevices()
    {
        QSignalBlocker blockXY(m_xyStageCombo);
        QSignalBlocker blockZ(m_zStageCombo);
        m_xyStageCombo->clear();
        m_zStageCombo->clear();
        m_xyStageCombo->addItems(m_scopeonecore->xyStageDevices());
        m_zStageCombo->addItems(m_scopeonecore->zStageDevices());

        // An empty current device matches no item and leaves the first one selected
        const int xyIndex = m_xyStageCombo->findText(m_scopeonecore->currentXYStageDevice());
        if (xyIndex >= 0)
        {
            m_xyStageCombo->setCurrentIndex(xyIndex);
        }
        const int zIndex = m_zStageCombo->findText(m_scopeonecore->currentFocusDevice());
        if (zIndex >= 0)
        {
            m_zStageCombo->setCurrentIndex(zIndex);
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
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 m_xyStepLineEdit, m_xyBigStepLineEdit, m_xyUpButton, m_xyDownButton, m_xyLeftButton,
                 m_xyRightButton, m_xyBigUpButton, m_xyBigDownButton, m_xyBigLeftButton, m_xyBigRightButton})
        {
            widget->setEnabled(hasXY);
        }
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 m_zStepLineEdit, m_zBigStepLineEdit, m_zUpButton, m_zDownButton, m_zBigUpButton, m_zBigDownButton})
        {
            widget->setEnabled(hasZ);
        }
    }

    // Reads current stage positions into labels
    void DeviceControlWidget::updateStagePositions()
    {
        const auto positionText = [](const char* axis, bool known, double value)
        {
            return QStringLiteral("%1: %2").arg(QLatin1String(axis),
                                                known ? QString::number(value, 'f', 2) : QStringLiteral("N/A"));
        };
        const QString xyLabel = selectedXYStageLabel();
        double x = 0.0;
        double y = 0.0;
        const bool hasXY = !xyLabel.isEmpty() && m_scopeonecore->readXYPosition(xyLabel, x, y);
        m_xPosLabel->setText(positionText("X", hasXY, x));
        m_yPosLabel->setText(positionText("Y", hasXY, y));

        const QString zLabel = selectedZStageLabel();
        double z = 0.0;
        const bool hasZ = !zLabel.isEmpty() && m_scopeonecore->readZPosition(zLabel, z);
        m_zPosLabel->setText(positionText("Z", hasZ, z));
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

        refreshCameraParameters();
    }

    // Applies exposure from the editor
    void DeviceControlWidget::onExposureChanged()
    {
        updateExposureLimits();

        bool ok = false;
        double exposureMs = m_exposureLineEdit->text().trimmed().toDouble(&ok);
        if (!ok)
        {
            refreshCameraParameters();
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
        m_cameraControlsGroup->setEnabled(canControlHardware);
        m_stageControlsGroup->setEnabled(m_liveViewerContext);
        m_cameraSelectCombo->setEnabled(canControlHardware && m_controlTargetEnabled);
        m_exposureLineEdit->setEnabled(canControlHardware);
        m_previewToggleButton->setEnabled(canControlHardware);
        m_snapButton->setEnabled(canControlHardware);
        m_previewToggleButton->setText(m_previewRunning
                                           ? QStringLiteral("Stop Preview")
                                           : QStringLiteral("Start Preview"));
        m_previewToggleButton->setStyleSheet(m_previewRunning && canControlHardware
                                                 ? QStringLiteral(
                                                     "QPushButton { background-color: #c0392b; color: white; font-weight: bold; }")
                                                 : QString());
        const bool hasRoiTarget = !roiCameraTarget().isEmpty();
        m_drawROIButton->setEnabled(canControlHardware && hasRoiTarget);
        m_halfROIButton->setEnabled(canControlHardware && hasRoiTarget);
        m_clearROIButton->setEnabled(canControlHardware && hasRoiTarget);

        updateStageControlsEnabled();
    }

    // Reads camera parameters back from hardware
    void DeviceControlWidget::refreshCameraParameters()
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
        // The range every targeted camera accepts, or the default when none reports one
        QStringList cameraIds;
        if (isAllTarget(m_currentTarget))
        {
            for (int i = 1; i < m_cameraSelectCombo->count(); ++i)
            {
                cameraIds.append(m_cameraSelectCombo->itemText(i));
            }
        }
        else
        {
            cameraIds.append(m_currentTarget);
        }
        bool hasLimits = false;
        double commonLower = 0.0;
        double commonUpper = 0.0;
        for (const QString& cameraId : cameraIds)
        {
            double lower = 0.0;
            double upper = 0.0;
            if (m_scopeonecore->getPropertyLimits(cameraId, QStringLiteral("Exposure"), lower, upper))
            {
                commonLower = hasLimits ? std::max(commonLower, lower) : lower;
                commonUpper = hasLimits ? std::min(commonUpper, upper) : upper;
                hasLimits = true;
            }
        }
        const bool valid = hasLimits && commonLower <= commonUpper;
        m_minExposureMs = valid ? commonLower : 0.1;
        m_maxExposureMs = valid ? commonUpper : 10000.0;
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
            m_cameraSelectCombo->addItems(cameraIds);
            // A single camera is selected directly, several start on All
            m_cameraSelectCombo->setCurrentIndex(cameraIds.size() == 1 ? 1 : 0);
        }

        onControlTargetSelectionChanged(m_cameraSelectCombo->currentText());
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
        m_currentTarget = target;
        updateControlsState();
        refreshCameraParameters();
        emit controlTargetChanged(target);
    }
} // namespace scopeone::ui
