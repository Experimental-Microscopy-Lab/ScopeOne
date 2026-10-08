#include "InspectWidget.h"
#include "ImageWorkspace.h"
#include "PreviewWidget.h"
#include "scopeone/ImageSceneModel.h"

#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QtConcurrent>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtMath>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace scopeone::ui
{
    namespace
    {
        // Return the short source label used by inspect groups
        QString inspectLayerSourceLabel(const QString& layerKey)
        {
            if (scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey))
            {
                return QStringLiteral("static");
            }
            if (scopeone::core::ScopeOneCore::isToolLayerKey(layerKey))
            {
                return QStringLiteral("tool");
            }
            return scopeone::core::ScopeOneCore::isProcessedLayerKey(layerKey)
                       ? QStringLiteral("proc")
                       : QStringLiteral("raw");
        }

        QString inspectLayerTitle(const QString& layerKey, bool active)
        {
            const QString cameraId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
            return QStringLiteral("%1 - %2 [%3]")
                .arg(active ? QStringLiteral("Active Layer") : QStringLiteral("Layer"),
                     cameraId,
                     inspectLayerSourceLabel(layerKey));
        }

        int framePixelValue(const scopeone::core::ImageFrame& frame, int x, int y)
        {
            const char* row = frame.bytes.constData() + static_cast<qint64>(y) * frame.stride;
            if (frame.isMono16())
            {
                quint16 value = 0;
                std::memcpy(&value, row + static_cast<qint64>(x) * 2, sizeof(value));
                return value;
            }
            return static_cast<unsigned char>(row[x]);
        }

        // Pixel statistics inside a rectangle, or along a line, of one mono frame
        InspectWidget::SelectionStatistics measureSelection(const scopeone::core::ImageFrame& frame,
                                                            const scopeone::core::ImageSceneModel::Markup& markup)
        {
            InspectWidget::SelectionStatistics stats;
            if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16()))
            {
                return stats;
            }
            const QRect bounds(0, 0, frame.width, frame.height);
            double sum = 0.0;
            double sumSquares = 0.0;
            const auto add = [&](int x, int y)
            {
                const int value = framePixelValue(frame, x, y);
                stats.minValue = stats.pixelCount == 0 ? value : std::min(stats.minValue, value);
                stats.maxValue = stats.pixelCount == 0 ? value : std::max(stats.maxValue, value);
                ++stats.pixelCount;
                sum += value;
                sumSquares += static_cast<double>(value) * value;
            };

            if (markup.type == scopeone::core::DocumentMarkupType::Line)
            {
                const QPoint delta = markup.end - markup.start;
                const int steps = std::max(std::abs(delta.x()), std::abs(delta.y()));
                for (int step = 0; step <= steps; ++step)
                {
                    const double t = steps == 0 ? 0.0 : static_cast<double>(step) / steps;
                    const int x = static_cast<int>(std::lround(markup.start.x() + t * delta.x()));
                    const int y = static_cast<int>(std::lround(markup.start.y() + t * delta.y()));
                    if (bounds.contains(x, y))
                    {
                        add(x, y);
                    }
                }
            }
            else
            {
                const QRect area = markup.rect.normalized().intersected(bounds);
                for (int y = area.top(); y <= area.bottom(); ++y)
                {
                    for (int x = area.left(); x <= area.right(); ++x)
                    {
                        add(x, y);
                    }
                }
            }

            if (stats.pixelCount > 0)
            {
                stats.mean = sum / static_cast<double>(stats.pixelCount);
                stats.stdDev = std::sqrt(std::max(0.0, sumSquares / static_cast<double>(stats.pixelCount)
                                                       - stats.mean * stats.mean));
            }
            return stats;
        }

        // Check whether a preview layer can use live core inspection
        bool isLiveLayerKey(const QString& layerKey)
        {
            return !scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey);
        }
    }

    class InspectCrossSectionWidget : public QWidget
    {
    public:
        explicit InspectCrossSectionWidget(QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setMinimumHeight(140);
            setMaximumHeight(180);
        }

        // Clear the current cross section plot
        void clear()
        {
            m_title.clear();
            m_values.clear();
            update();
        }

        // Store a new layer cross section profile for painting
        void setProfile(const QString& layerKey, const QVector<int>& values)
        {
            m_title = layerKey;
            m_values = values;
            update();
        }

        QVector<int> values() const
        {
            return m_values;
        }

    protected:
        // Paint the cross section curve and its summary labels
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            const QPalette& colors = palette();
            painter.fillRect(rect(), colors.brush(QPalette::Base));
            painter.setRenderHint(QPainter::Antialiasing, true);

            if (m_values.isEmpty())
            {
                painter.setPen(colors.color(QPalette::PlaceholderText));
                painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Draw a line to plot its profile"));
                return;
            }

            const QRect plotRect = rect().adjusted(40, 24, -12, -28);
            painter.setPen(colors.color(QPalette::Mid));
            painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());
            painter.drawLine(plotRect.bottomLeft(), plotRect.topLeft());

            int minValue = m_values.first();
            int maxValue = m_values.first();
            for (int value : m_values)
            {
                minValue = qMin(minValue, value);
                maxValue = qMax(maxValue, value);
            }
            const int valueRange = qMax(1, maxValue - minValue);

            painter.setPen(colors.color(QPalette::Text));
            painter.drawText(QRect(8, 4, width() - 16, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1  N=%2  Min=%3  Max=%4")
                             .arg(m_title)
                             .arg(m_values.size())
                             .arg(minValue)
                             .arg(maxValue));

            painter.drawText(QRect(0, plotRect.top() - 6, 36, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(maxValue));
            painter.drawText(QRect(0, plotRect.bottom() - 8, 36, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(minValue));
            painter.drawText(QRect(plotRect.left(), plotRect.bottom() + 6, 80, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("0"));
            painter.drawText(QRect(plotRect.right() - 80, plotRect.bottom() + 6, 80, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(qMax(0, m_values.size() - 1)));
            painter.drawText(QRect(plotRect.left(), plotRect.bottom() + 6, plotRect.width(), 16),
                             Qt::AlignCenter | Qt::AlignVCenter,
                             QStringLiteral("Position"));

            painter.save();
            painter.translate(12, plotRect.center().y());
            painter.rotate(-90.0);
            painter.drawText(QRect(-plotRect.height() / 2, -10, plotRect.height(), 16),
                             Qt::AlignCenter | Qt::AlignVCenter,
                             QStringLiteral("Intensity"));
            painter.restore();

            QPolygonF line;
            line.reserve(m_values.size());
            const int pointCount = m_values.size();
            for (int i = 0; i < pointCount; ++i)
            {
                const double x = (pointCount == 1)
                                     ? plotRect.center().x()
                                     : plotRect.left() + (static_cast<double>(i) * plotRect.width()) / static_cast<
                                         double>(pointCount - 1);
                const double yNorm = static_cast<double>(m_values.at(i) - minValue) / static_cast<double>(valueRange);
                const double y = plotRect.bottom() - yNorm * plotRect.height();
                line << QPointF(x, y);
            }

            painter.setPen(QPen(colors.color(QPalette::Highlight), 1.5));
            if (line.size() == 1)
            {
                painter.drawEllipse(line.first(), 2.0, 2.0);
            }
            else
            {
                painter.drawPolyline(line);
            }
        }

    private:
        QString m_title;
        QVector<int> m_values;
    };

    // Create the inspection panel and subscribe to core analysis signals
    InspectWidget::InspectWidget(scopeone::core::ScopeOneCore* core,
                                 ImageWorkspace* workspace,
                                 QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
          , m_workspace(workspace)
    {
        setWindowTitle(QStringLiteral("Inspect"));
        setupUI();
        updateControlsState();

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::layerHistogramReady,
                this, &InspectWidget::setLayerInspect);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::layerAnalysisCleared,
                this, &InspectWidget::clearLayerInspect);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::layerLineProfileUpdated,
                this, [this](const QString& layerKey, const QVector<int>& values)
                {
                    m_crossSectionWidget->setProfile(layerKey, values);
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::lineProfileCleared,
                this, [this]() { m_crossSectionWidget->clear(); });
        connect(m_workspace, &ImageWorkspace::lineProfileUpdated,
                this, [this](const QString& layerKey, const QVector<int>& values)
                {
                    m_crossSectionWidget->setProfile(layerKey, values);
                });
        // Re-measure the selection whenever its layer shows a new frame, at the line profile rate
        const auto refreshForLayer = [this](const QString& layerKey)
        {
            scopeone::core::ImageSceneModel::Markup markup;
            if (selectedMarkup(markup) && markup.layerKey == layerKey)
            {
                scheduleSelectionRefresh();
            }
        };
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewRawFrameReady, this,
                [refreshForLayer](const scopeone::core::ImageFrame& frame)
                {
                    refreshForLayer(scopeone::core::ScopeOneCore::rawLayerKey(frame.cameraId));
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewProcessedFrameReady, this,
                [refreshForLayer](const scopeone::core::ImageFrame& frame)
                {
                    refreshForLayer(scopeone::core::ScopeOneCore::processedLayerKey(frame.cameraId));
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::previewToolFrameReady, this,
                [refreshForLayer](const scopeone::core::ImageFrame& frame)
                {
                    refreshForLayer(scopeone::core::ScopeOneCore::toolLayerKey(frame.cameraId));
                });
        connect(m_workspace, &ImageWorkspace::activeViewerChanged,
                this, &InspectWidget::refreshActiveViewer);
        connect(m_workspace, &ImageWorkspace::activeFrameChanged,
                this, [this]()
                {
                    if (currentLayerKey().isEmpty())
                    {
                        return;
                    }
                    m_workspace->requestHistogram(currentLayerKey());
                    scheduleSelectionRefresh();
                });
        connect(m_workspace, &ImageWorkspace::activeLayerChanged,
                this, [this](const QString&)
                {
                    const QString layerKey = currentLayerKey();
                    if (!layerKey.isEmpty())
                    {
                        m_workspace->requestHistogram(layerKey);
                    }
                    updateLayerVisibility();
                    updateControlsState();
                });
        refreshActiveViewer();
    }

    void InspectWidget::refreshActiveViewer()
    {
        saveViewerState();
        const QString viewerStateId = m_workspace->activeDocumentId();
        m_activeViewerStateId = viewerStateId;
        restoreViewerState();
        if (m_sceneModel)
        {
            disconnect(m_sceneModel, nullptr, this, nullptr);
        }
        m_sceneModel = m_workspace->activeSceneModel();
        setAvailableLayers(m_sceneModel ? m_sceneModel->layerIds() : QStringList{});
        if (!m_sceneModel)
        {
            return;
        }
        const auto refreshLayerDisplay = [this](const QString& layerKey)
        {
            const auto state = m_layerStates.constFind(layerKey);
            if (state != m_layerStates.constEnd() && state->hasStats)
            {
                updateLayerInspect(layerKey, state->stats);
            }
        };
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layerDisplayChanged,
                this, [this, refreshLayerDisplay](const QString& layerKey)
                {
                    refreshLayerDisplay(layerKey);
                    updateLayerVisibility();
                    updateControlsState();
                });
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layerAutoStretchChanged,
                this, [refreshLayerDisplay](const QString& layerKey, bool)
                {
                    refreshLayerDisplay(layerKey);
                });
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::layersChanged,
                this, [this]()
                {
                    setAvailableLayers(m_sceneModel->layerIds());
                });
        connect(m_sceneModel, &scopeone::core::ImageSceneModel::markupsChanged,
                this, &InspectWidget::scheduleSelectionRefresh);
        const QString layerKey = currentLayerKey();
        if (!layerKey.isEmpty())
        {
            m_workspace->requestHistogram(layerKey);
        }
        updateLayerVisibility();
        for (auto it = m_layerStates.cbegin(); it != m_layerStates.cend(); ++it)
        {
            if (it->hasStats)
            {
                updateLayerInspect(it.key(), it->stats);
            }
        }
        refreshSelection();
    }

    void InspectWidget::saveViewerState()
    {
        m_viewerStates[m_activeViewerStateId].layerStates = m_layerStates;
    }

    void InspectWidget::restoreViewerState()
    {
        const auto it = m_viewerStates.constFind(m_activeViewerStateId);
        m_layerStates = it == m_viewerStates.constEnd() ? QHash<QString, LayerInspectState>{}
                                                        : it->layerStates;
    }

    InspectWidget::~InspectWidget()
    {
        m_scopeonecore->setActiveHistogramLayer({});
    }

    // Enable inspect controls when camera state changes
    void InspectWidget::onCameraInitialized(bool initialized)
    {
        m_cameraInitialized = initialized;
        updateControlsState();
    }

    // Remove inspect state for layers that are no longer available
    void InspectWidget::setAvailableLayers(const QStringList& layerKeys)
    {
        m_availableLayerKeys = layerKeys;

        for (auto it = m_layerStates.begin(); it != m_layerStates.end();)
        {
            if (!m_availableLayerKeys.contains(it.key()))
            {
                it = m_layerStates.erase(it);
            }
            else
            {
                ++it;
            }
        }

        QList<QString> keysToRemove;
        for (auto it = m_layerInfoGroups.begin(); it != m_layerInfoGroups.end(); ++it)
        {
            if (!m_availableLayerKeys.contains(it.key()))
            {
                keysToRemove.append(it.key());
            }
        }
        for (const QString& key : keysToRemove)
        {
            removeLayerInfo(key);
        }

        for (const QString& key : m_availableLayerKeys)
        {
            addLayerInfo(key);
        }

        if (!currentLayerKey().isEmpty() && !m_availableLayerKeys.contains(currentLayerKey()))
        {
            m_scopeonecore->setActiveHistogramLayer({});
        }
        updateLayerVisibility();
        updateControlsState();

        if (!currentLayerKey().isEmpty())
        {
            m_workspace->requestHistogram(currentLayerKey());
        }
    }

    // Store live camera availability for core backed tools
    void InspectWidget::setAvailableCameras(const QStringList& cameraIds)
    {
        m_availableCameraIds = cameraIds;

        if (!currentLayerKey().isEmpty()
            && isLiveLayerKey(currentLayerKey())
            && !m_availableCameraIds.contains(currentLayerCameraId()))
        {
            m_scopeonecore->setActiveHistogramLayer({});
        }
        updateLayerVisibility();
        updateControlsState();
    }

    // Show inspect data for an explicit preview layer
    void InspectWidget::setLayerInspect(
        const QString& layerKey,
        const scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty() || !stats.hasData())
        {
            return;
        }

        if (!m_layerInfoGroups.contains(trimmedLayerKey))
        {
            addLayerInfo(trimmedLayerKey);
        }
        updateLayerInspect(trimmedLayerKey, stats);
    }

    // Remove cached inspect data for one graph layer
    void InspectWidget::clearLayerInspect(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty())
        {
            return;
        }

        m_layerStates.remove(trimmedLayerKey);
        removeLayerInfo(trimmedLayerKey);
        scheduleSelectionRefresh();
        updateLayerVisibility();
        updateControlsState();
    }





    // Build the scrollable inspect panel
    void InspectWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setSpacing(0);
        mainLayout->setContentsMargins(0, 0, 0, 0);

        auto* scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setFrameShape(QFrame::NoFrame);

        auto* contentContainer = new QWidget(scrollArea);
        m_contentContainer = contentContainer;
        auto* contentLayout = new QVBoxLayout(contentContainer);
        m_contentLayout = contentLayout;
        contentLayout->setSpacing(8);
        contentLayout->setContentsMargins(5, 5, 5, 5);

        // Selection: shape tools and what the selected shape covers
        auto* selectionGroup = new QGroupBox(QStringLiteral("Selection"), contentContainer);
        auto* selectionLayout = new QVBoxLayout(selectionGroup);
        auto* toolButtons = new QHBoxLayout();
        m_rectangleToolButton = new QPushButton(QStringLiteral("Rectangle"), selectionGroup);
        m_rectangleToolButton->setToolTip(QStringLiteral("Drag on the image to draw a rectangle"));
        m_lineToolButton = new QPushButton(QStringLiteral("Line"), selectionGroup);
        m_lineToolButton->setToolTip(QStringLiteral("Drag on the image to draw a line and plot its profile"));
        m_deleteSelectionButton = new QPushButton(QStringLiteral("Delete"), selectionGroup);
        m_deleteSelectionButton->setToolTip(QStringLiteral("Delete the selected shape (Del)"));
        for (QPushButton* button : {m_rectangleToolButton, m_lineToolButton})
        {
            button->setCheckable(true);
            toolButtons->addWidget(button);
        }
        toolButtons->addStretch();
        toolButtons->addWidget(m_deleteSelectionButton);
        selectionLayout->addLayout(toolButtons);
        m_selectionInfoLabel = new QLabel(selectionGroup);
        m_selectionInfoLabel->setTextFormat(Qt::PlainText);
        m_selectionInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_selectionInfoLabel->setWordWrap(true);
        selectionLayout->addWidget(m_selectionInfoLabel);

        // Live values sit in fixed right-aligned cells so changing digits do not shift the text
        m_selectionStatsWidget = new QWidget(selectionGroup);
        auto* statsLayout = new QGridLayout(m_selectionStatsWidget);
        statsLayout->setContentsMargins(0, 0, 0, 0);
        const int valueWidth = m_selectionStatsWidget->fontMetrics().horizontalAdvance(QStringLiteral("65535.0")) + 4;
        const auto addValue = [this, statsLayout, valueWidth](const QString& name, int row, int column)
        {
            statsLayout->addWidget(new QLabel(name, m_selectionStatsWidget), row, column);
            auto* value = new QLabel(m_selectionStatsWidget);
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            value->setMinimumWidth(valueWidth);
            statsLayout->addWidget(value, row, column + 1);
            return value;
        };
        m_selectionMeanLabel = addValue(QStringLiteral("Mean:"), 0, 0);
        m_selectionStdDevLabel = addValue(QStringLiteral("Std Dev:"), 0, 2);
        m_selectionMinLabel = addValue(QStringLiteral("Min:"), 1, 0);
        m_selectionMaxLabel = addValue(QStringLiteral("Max:"), 1, 2);
        statsLayout->setColumnStretch(4, 1);
        selectionLayout->addWidget(m_selectionStatsWidget);
        contentLayout->addWidget(selectionGroup);

        // Profile: intensity along the selected line
        auto* profileGroup = new QGroupBox(QStringLiteral("Profile"), contentContainer);
        auto* profileLayout = new QVBoxLayout(profileGroup);
        m_crossSectionWidget = new InspectCrossSectionWidget(profileGroup);
        profileLayout->addWidget(m_crossSectionWidget);
        contentLayout->addWidget(profileGroup);

        contentLayout->addStretch();

        // Selection follows mouse drags and live frames; coalesce to the line profile refresh rate
        m_selectionRefreshTimer = new QTimer(this);
        m_selectionRefreshTimer->setSingleShot(true);
        m_selectionRefreshTimer->setInterval(16);
        connect(m_selectionRefreshTimer, &QTimer::timeout, this, &InspectWidget::refreshSelection);
        m_selectionStatsWatcher = new QFutureWatcher<SelectionStatistics>(this);
        connect(m_selectionStatsWatcher, &QFutureWatcher<SelectionStatistics>::finished,
                this, &InspectWidget::applySelectionStats);

        // Clicking the checked tool again returns to plain selection
        const auto setTool = [this](PreviewWidget::AnalysisTool tool)
        {
            m_rectangleToolButton->setChecked(tool == PreviewWidget::AnalysisTool::Rectangle);
            m_lineToolButton->setChecked(tool == PreviewWidget::AnalysisTool::Line);
            if (PreviewWidget* preview = m_workspace->activePreviewWidget())
            {
                preview->setAnalysisTool(tool);
            }
        };
        connect(m_rectangleToolButton, &QPushButton::clicked, this, [setTool](bool checked)
        {
            setTool(checked ? PreviewWidget::AnalysisTool::Rectangle : PreviewWidget::AnalysisTool::Select);
        });
        connect(m_lineToolButton, &QPushButton::clicked, this, [setTool](bool checked)
        {
            setTool(checked ? PreviewWidget::AnalysisTool::Line : PreviewWidget::AnalysisTool::Select);
        });
        connect(m_deleteSelectionButton, &QPushButton::clicked, this, [this]()
        {
            scopeone::core::ImageSceneModel::Markup markup;
            if (selectedMarkup(markup))
            {
                m_sceneModel->remove(markup.id);
            }
        });

        scrollArea->setWidget(contentContainer);
        mainLayout->addWidget(scrollArea);
    }

    // Create statistics controls for one layer
    QWidget* InspectWidget::createLayerInfoGroup(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        auto* group = new QGroupBox(inspectLayerTitle(normalizedLayerKey, false), m_contentContainer);
        auto* layout = new QVBoxLayout(group);
        LayerInfoGroup infoGroup;
        infoGroup.layerKey = normalizedLayerKey;
        infoGroup.groupBox = group;

        layout->addWidget(createStatisticsGroup(infoGroup));
        m_layerInfoGroups.insert(normalizedLayerKey, infoGroup);
        m_contentLayout->insertWidget(m_contentLayout->count() - 1, group);

        return group;
    }

    // Create labels for per layer image statistics
    QWidget* InspectWidget::createStatisticsGroup(LayerInfoGroup& infoGroup)
    {
        auto* group = new QWidget(this);
        auto* layout = new QGridLayout(group);
        layout->setContentsMargins(0, 0, 0, 0);
        // Right-aligned values with a fixed minimum width keep the grid steady during live updates
        const int valueMinWidth = group->fontMetrics().horizontalAdvance(QStringLiteral("65535.0")) + 4;
        const auto makeValueLabel = [group, valueMinWidth](const QString& text)
        {
            auto* label = new QLabel(text, group);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            label->setMinimumWidth(valueMinWidth);
            return label;
        };

        layout->addWidget(new QLabel(QStringLiteral("Mean:"), group), 0, 0);
        auto* meanLabel = makeValueLabel(QStringLiteral("0.0"));
        layout->addWidget(meanLabel, 0, 1);

        layout->addWidget(new QLabel(QStringLiteral("Min:"), group), 0, 2);
        auto* minLabel = makeValueLabel(QStringLiteral("0"));
        layout->addWidget(minLabel, 0, 3);

        layout->addWidget(new QLabel(QStringLiteral("Max:"), group), 1, 0);
        auto* maxLabel = makeValueLabel(QStringLiteral("0"));
        layout->addWidget(maxLabel, 1, 1);

        layout->addWidget(new QLabel(QStringLiteral("Std Dev:"), group), 1, 2);
        auto* stdDevLabel = makeValueLabel(QStringLiteral("0.0"));
        layout->addWidget(stdDevLabel, 1, 3);

        layout->addWidget(new QLabel(QStringLiteral("Pixels:"), group), 2, 0);
        auto* pixelCountLabel = makeValueLabel(QStringLiteral("0"));
        layout->addWidget(pixelCountLabel, 2, 1);
        layout->setColumnStretch(4, 1);

        infoGroup.meanLabel = meanLabel;
        infoGroup.minLabel = minLabel;
        infoGroup.maxLabel = maxLabel;
        infoGroup.stdDevLabel = stdDevLabel;
        infoGroup.pixelCountLabel = pixelCountLabel;

        return group;
    }

    // Add a layer group if it is not already visible
    void InspectWidget::addLayerInfo(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        if (m_layerInfoGroups.contains(normalizedLayerKey))
        {
            return;
        }

        createLayerInfoGroup(normalizedLayerKey);
        updateLayerVisibility();
        updateControlsState();
    }

    // Remove a layer group and its widgets
    void InspectWidget::removeLayerInfo(const QString& layerKey)
    {
        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }

        LayerInfoGroup& infoGroup = it.value();
        m_contentLayout->removeWidget(infoGroup.groupBox);
        infoGroup.groupBox->deleteLater();
        m_layerInfoGroups.erase(it);
    }

    // Synchronize histogram controls with fresh statistics
    void InspectWidget::updateLayerInspect(
        const QString& layerKey,
        const scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        auto it = m_layerInfoGroups.find(normalizedLayerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }

        LayerInspectState& state = getOrCreateLayerState(normalizedLayerKey);
        state.stats = stats;
        state.hasStats = stats.hasData();
        if (!state.hasStats)
        {
            return;
        }

        updateStatisticsDisplay(normalizedLayerKey, stats);
        updateControlsState();
    }

    // Update numeric statistics labels for one layer
    void InspectWidget::updateStatisticsDisplay(
        const QString& layerKey,
        const scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }
        LayerInfoGroup& infoGroup = it.value();

        if (stats.bitDepth > 8)
        {
            infoGroup.meanLabel->setText(QString::number(stats.mean, 'f', 0));
            infoGroup.minLabel->setText(QString::number(static_cast<int>(stats.minVal)));
            infoGroup.maxLabel->setText(QString::number(static_cast<int>(stats.maxVal)));
            infoGroup.stdDevLabel->setText(QString::number(stats.stdDev, 'f', 0));
        }
        else
        {
            infoGroup.meanLabel->setText(QString::number(stats.mean, 'f', 1));
            infoGroup.minLabel->setText(QString::number(static_cast<int>(stats.minVal)));
            infoGroup.maxLabel->setText(QString::number(static_cast<int>(stats.maxVal)));
            infoGroup.stdDevLabel->setText(QString::number(stats.stdDev, 'f', 1));
        }

        infoGroup.pixelCountLabel->setText(QLocale().toString(static_cast<qlonglong>(stats.totalPixels)));
    }

    // Enable controls according to the current selection
    void InspectWidget::updateControlsState()
    {
        scopeone::core::ImageSceneModel::Markup markup;
        m_deleteSelectionButton->setEnabled(selectedMarkup(markup));
    }

    void InspectWidget::scheduleSelectionRefresh()
    {
        if (!m_selectionRefreshTimer->isActive())
        {
            m_selectionRefreshTimer->start();
        }
    }

    // Re-measure the selected shape and update the selection and profile views
    void InspectWidget::refreshSelection()
    {
        scopeone::core::ImageSceneModel::Markup markup;
        const bool hasSelection = selectedMarkup(markup);
        m_selectionInfoLabel->setText(
            hasSelection ? selectionSummary(markup).join(QLatin1Char('\n'))
                         : QStringLiteral("Choose Rectangle or Line, then drag on the image."));

        if (hasSelection)
        {
            requestSelectionStats(markup);
        }
        else
        {
            m_selectionStatsMarkupId.clear();
            m_selectionStatsWidget->hide();
        }
        updateControlsState();
    }

    // Measure the selection on a worker thread; while one runs, only remember that another is due
    void InspectWidget::requestSelectionStats(const scopeone::core::ImageSceneModel::Markup& markup)
    {
        if (m_selectionStatsWatcher->isRunning())
        {
            m_selectionStatsPending = true;
            return;
        }
        const scopeone::core::ImageFrame frame = m_workspace->frameForLayer(markup.layerKey);
        m_selectionStatsMarkupId = markup.id;
        m_selectionStatsWatcher->setFuture(QtConcurrent::run([frame, markup]()
        {
            return measureSelection(frame, markup);
        }));
    }

    // Show finished statistics if they still belong to the selected shape, then catch up
    void InspectWidget::applySelectionStats()
    {
        scopeone::core::ImageSceneModel::Markup markup;
        const bool current = selectedMarkup(markup) && markup.id == m_selectionStatsMarkupId;
        const SelectionStatistics stats = m_selectionStatsWatcher->result();
        if (current)
        {
            m_selectionStatsWidget->setVisible(stats.pixelCount > 0);
            if (stats.pixelCount > 0)
            {
                m_selectionMeanLabel->setText(QString::number(stats.mean, 'f', 1));
                m_selectionStdDevLabel->setText(QString::number(stats.stdDev, 'f', 1));
                m_selectionMinLabel->setText(QString::number(stats.minValue));
                m_selectionMaxLabel->setText(QString::number(stats.maxValue));
            }
        }
        if (m_selectionStatsPending)
        {
            m_selectionStatsPending = false;
            if (selectedMarkup(markup))
            {
                requestSelectionStats(markup);
            }
        }
    }

    bool InspectWidget::selectedMarkup(scopeone::core::ImageSceneModel::Markup& outMarkup) const
    {
        if (!m_sceneModel)
        {
            return false;
        }
        for (const scopeone::core::ImageSceneModel::Markup& markup : m_sceneModel->markups())
        {
            if (markup.selected)
            {
                outMarkup = markup;
                return true;
            }
        }
        return false;
    }

    // Geometry of one shape in pixels and calibrated units
    QStringList InspectWidget::selectionSummary(const scopeone::core::ImageSceneModel::Markup& markup) const
    {
        const QRect rect = markup.rect.normalized();

        // Calibrated values follow the layer's pixel to sensor transform and the camera pixel size
        const double pixelSizeUm = m_workspace->pixelSizeUm(markup.layerKey);
        scopeone::core::DocumentLayer layer;
        const bool calibrated = pixelSizeUm > 0.0
            && m_sceneModel
            && m_sceneModel->findLayer(markup.layerKey, layer);

        QStringList lines{markup.layerKey};
        if (markup.type == scopeone::core::DocumentMarkupType::Line)
        {
            const double dx = markup.end.x() - markup.start.x();
            const double dy = markup.end.y() - markup.start.y();
            const double angle = qRadiansToDegrees(std::atan2(-dy, dx));
            lines.append(QStringLiteral("Start (%1, %2)   End (%3, %4)")
                             .arg(markup.start.x()).arg(markup.start.y())
                             .arg(markup.end.x()).arg(markup.end.y()));
            QString length = QStringLiteral("Length %1 px").arg(std::hypot(dx, dy), 0, 'f', 2);
            if (calibrated)
            {
                const QPointF sensorStart = layer.pixelToSensor.map(QPointF(markup.start));
                const QPointF sensorEnd = layer.pixelToSensor.map(QPointF(markup.end));
                length += QStringLiteral("  ·  %1 µm")
                              .arg(std::hypot(sensorEnd.x() - sensorStart.x(),
                                              sensorEnd.y() - sensorStart.y()) * pixelSizeUm,
                                   0, 'f', 3);
            }
            lines.append(length + QStringLiteral("   Angle %1°").arg(angle < 0.0 ? angle + 360.0 : angle, 0, 'f', 1));
        }
        else
        {
            lines.append(QStringLiteral("X %1   Y %2   W %3   H %4")
                             .arg(rect.x()).arg(rect.y())
                             .arg(rect.width()).arg(rect.height()));
            QString area = QStringLiteral("Area %1 px")
                               .arg(QLocale().toString(static_cast<qlonglong>(rect.width()) * rect.height()));
            if (calibrated)
            {
                const double scale = std::abs(layer.pixelToSensor.m11 * layer.pixelToSensor.m22
                                              - layer.pixelToSensor.m12 * layer.pixelToSensor.m21);
                area += QStringLiteral("  ·  %1 µm²")
                            .arg(static_cast<double>(rect.width()) * rect.height()
                                     * scale * pixelSizeUm * pixelSizeUm,
                                 0, 'f', 3);
            }
            lines.append(area);
        }
        return lines;
    }

    // Shows inspect controls for the selected preview layer
    void InspectWidget::updateLayerVisibility()
    {
        const QString layerKey = currentLayerKey();
        const QStringList visibleLayerKeys = m_sceneModel
                                                 ? m_sceneModel->visibleLayerIds()
                                                 : QStringList{};
        for (auto it = m_layerInfoGroups.begin(); it != m_layerInfoGroups.end(); ++it)
        {
            LayerInfoGroup& infoGroup = it.value();
            const bool showLayer = visibleLayerKeys.contains(infoGroup.layerKey)
                                   && infoGroup.layerKey == layerKey;
            infoGroup.groupBox->setVisible(showLayer);
            infoGroup.groupBox->setTitle(
                inspectLayerTitle(infoGroup.layerKey, infoGroup.layerKey == layerKey));
        }
    }

    // Return persistent inspect state for one preview layer
    InspectWidget::LayerInspectState& InspectWidget::getOrCreateLayerState(const QString& layerKey)
    {
        auto it = m_layerStates.find(layerKey);
        if (it == m_layerStates.end())
        {
            LayerInspectState state;
            state.layerKey = layerKey;
            it = m_layerStates.insert(layerKey, state);
        }
        return it.value();
    }

    QString InspectWidget::currentLayerCameraId() const
    {
        return scopeone::core::ScopeOneCore::sourceIdFromLayerKey(currentLayerKey());
    }

    QString InspectWidget::currentLayerKey() const
    {
        return m_workspace ? m_workspace->activeLayerKey() : QString{};
    }
} // namespace scopeone::ui
