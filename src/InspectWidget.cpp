#include "InspectWidget.h"
#include "ImageWorkspace.h"
#include "scopeone/ImageSceneModel.h"

#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QtMath>
#include <QtGlobal>
#include <cmath>

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
                painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No cross section"));
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
                this, &InspectWidget::setLayerCrossSectionProfile);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::lineProfileCleared,
                this, &InspectWidget::clearCrossSectionProfile);
        connect(m_workspace, &ImageWorkspace::activeViewerChanged,
                this, &InspectWidget::refreshActiveViewer);
        connect(m_workspace, &ImageWorkspace::activeFrameChanged,
                this, [this]()
                {
                    if (m_workspace->isLiveViewerActive() || currentLayerKey().isEmpty())
                    {
                        return;
                    }
                    m_workspace->requestHistogram(currentLayerKey());
                });
        connect(m_workspace, &ImageWorkspace::activeLayerChanged,
                this, [this](const QString&)
                {
                    const QString layerKey = currentLayerKey();
                    if (m_workspace->isLiveViewerActive())
                    {
                        m_scopeonecore->setActiveHistogramLayer(layerKey);
                    }
                    else if (!layerKey.isEmpty())
                    {
                        m_workspace->requestHistogram(layerKey);
                    }
                    updateLayerVisibility();
                    updateControlsState();
                });
        connect(m_workspace, &ImageWorkspace::histogramReady,
                this, [this](const QString& layerKey,
                             const scopeone::core::ScopeOneCore::HistogramStats& stats)
                {
                    if (layerKey == currentLayerKey() && !m_workspace->isLiveViewerActive())
                    {
                        setLayerInspect(layerKey, stats);
                        if (m_workspace->layerAutoStretchEnabled(layerKey))
                        {
                            m_sceneModel->setLayerDisplayLevels(
                                layerKey,
                                stats.autoMinLevel,
                                stats.autoMaxLevel,
                                stats.maxValue);
                        }
                    }
                });
        connect(m_workspace, &ImageWorkspace::lineProfileUpdated,
                this, &InspectWidget::setLayerCrossSectionProfile);
        refreshActiveViewer();
    }

    void InspectWidget::refreshActiveViewer()
    {
        saveViewerState();
        const bool inspectLive = m_workspace->isLiveViewerActive();
        const QString viewerStateId = m_workspace->activeDocumentId();
        if (m_inspectingLive && !inspectLive)
        {
            m_scopeonecore->setActiveHistogramLayer({});
        }
        m_inspectingLive = inspectLive;
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
        const QString layerKey = currentLayerKey();
        if (m_workspace->isLiveViewerActive())
        {
            m_scopeonecore->setActiveHistogramLayer(layerKey);
        }
        else if (!layerKey.isEmpty())
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
        updateControlsState();
    }

    void InspectWidget::saveViewerState()
    {
        ViewerInspectState& state = m_viewerStates[m_activeViewerStateId];
        state.layerStates = m_layerStates;
        state.crossSectionLayerKey = m_crossSectionLayerKey;
        state.crossSectionValues = m_crossSectionWidget->values();
        state.measurementLayerKey = m_measurementLayerKey;
        state.measurementInfo = m_measurementInfoLabel->text();
    }

    void InspectWidget::restoreViewerState()
    {
        const auto it = m_viewerStates.constFind(m_activeViewerStateId);
        if (it == m_viewerStates.constEnd())
        {
            m_layerStates.clear();
            m_crossSectionLayerKey.clear();
            m_crossSectionWidget->clear();
            m_measurementLayerKey.clear();
            m_measurementInfoLabel->clear();
            m_measurementInfoLabel->hide();
            return;
        }

        const ViewerInspectState& state = it.value();
        m_layerStates = state.layerStates;
        m_crossSectionLayerKey = state.crossSectionLayerKey;
        if (state.crossSectionLayerKey.isEmpty() || state.crossSectionValues.isEmpty())
        {
            m_crossSectionWidget->clear();
        }
        else
        {
            m_crossSectionWidget->setProfile(
                state.crossSectionLayerKey, state.crossSectionValues);
        }
        m_measurementLayerKey = state.measurementLayerKey;
        m_measurementInfoLabel->setText(state.measurementInfo);
        m_measurementInfoLabel->setVisible(!state.measurementInfo.isEmpty());
    }

    InspectWidget::~InspectWidget()
    {
        if (m_workspace->isLiveViewerActive())
        {
            m_scopeonecore->setActiveHistogramLayer({});
        }
    }

    // Enable inspect controls when camera state changes
    void InspectWidget::onCameraInitialized(bool initialized)
    {
        m_cameraInitialized = initialized;
        updateControlsState();

        if (!initialized)
        {
            clearCrossSectionProfile();
        }
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

        if (!m_crossSectionLayerKey.isEmpty() && !m_availableLayerKeys.contains(m_crossSectionLayerKey))
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        }
        if (!m_measurementLayerKey.isEmpty() && !m_availableLayerKeys.contains(m_measurementLayerKey))
        {
            clearMeasurementLine();
        }

        if (!currentLayerKey().isEmpty() && !m_availableLayerKeys.contains(currentLayerKey()))
        {
            if (m_workspace->isLiveViewerActive())
            {
                m_scopeonecore->setActiveHistogramLayer({});
            }
            clearCrossSectionProfile();
        }
        updateLayerVisibility();
        updateControlsState();

        if (m_workspace->isLiveViewerActive())
        {
            m_scopeonecore->setActiveHistogramLayer(currentLayerKey());
        }
    }

    // Store live camera availability for core backed tools
    void InspectWidget::setAvailableCameras(const QStringList& cameraIds)
    {
        m_availableCameraIds = cameraIds;

        if (!m_crossSectionLayerKey.isEmpty()
            && isLiveLayerKey(m_crossSectionLayerKey)
            && !m_availableCameraIds.contains(
                scopeone::core::ScopeOneCore::sourceIdFromLayerKey(m_crossSectionLayerKey)))
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        }

        if (!currentLayerKey().isEmpty()
            && isLiveLayerKey(currentLayerKey())
            && !m_availableCameraIds.contains(currentLayerCameraId()))
        {
            if (m_workspace->isLiveViewerActive())
            {
                m_scopeonecore->setActiveHistogramLayer({});
            }
            clearCrossSectionProfile();
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
        if (currentLayerKey() == trimmedLayerKey)
        {
            clearCrossSectionProfile();
        }
        if (m_measurementLayerKey == trimmedLayerKey)
        {
            clearMeasurementLine();
        }
        updateLayerVisibility();
        updateControlsState();
    }

    // Clear the cross section plot
    void InspectWidget::clearCrossSectionProfile()
    {
        m_crossSectionLayerKey.clear();
        m_crossSectionWidget->clear();
    }

    // Display one line measurement for the inspected layer
    void InspectWidget::setMeasurementLine(const QString& layerKey,
                                           const QPoint& start,
                                           const QPoint& end,
                                           double actualLengthUm)
    {
        m_measurementLayerKey = layerKey.trimmed();
        const double dx = static_cast<double>(end.x() - start.x());
        const double dy = static_cast<double>(end.y() - start.y());
        const double lengthPixels = std::hypot(dx, dy);
        double angleDegrees = std::atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
        if (angleDegrees < 0.0)
        {
            angleDegrees += 360.0;
        }

        QStringList lines{
            QStringLiteral("Layer: %1").arg(m_measurementLayerKey),
            QStringLiteral("Start: (%1, %2)").arg(start.x()).arg(start.y()),
            QStringLiteral("Angle: %1°").arg(angleDegrees, 0, 'f', 1),
            QStringLiteral("Length: %1 px").arg(lengthPixels, 0, 'f', 2)
        };
        if (actualLengthUm > 0.0)
        {
            lines.append(QStringLiteral("Actual: %1 µm")
                             .arg(actualLengthUm, 0, 'f', 3));
        }
        else
        {
            lines.append(QStringLiteral("Scale: not calibrated"));
        }
        m_measurementInfoLabel->setText(lines.join('\n'));
        m_measurementInfoLabel->show();
    }

    // Clear the current line measurement display
    void InspectWidget::clearMeasurementLine()
    {
        m_measurementLayerKey.clear();
        m_measurementInfoLabel->clear();
        m_measurementInfoLabel->hide();
    }

    // Display a freshly computed cross section profile for one layer
    void InspectWidget::setLayerCrossSectionProfile(const QString& layerKey, const QVector<int>& values)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty() || trimmedLayerKey != currentLayerKey())
        {
            return;
        }

        m_crossSectionLayerKey = trimmedLayerKey;
        m_crossSectionWidget->setProfile(trimmedLayerKey, values);
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
        auto* contentLayout = new QVBoxLayout(contentContainer);
        contentLayout->setSpacing(8);
        contentLayout->setContentsMargins(5, 5, 5, 5);

        auto* annotationGroup = new QGroupBox(QStringLiteral("Annotation"), contentContainer);
        auto* annotationLayout = new QVBoxLayout(annotationGroup);
        auto* annotationButtons = new QHBoxLayout();
        m_drawMeasurementLineButton = new QPushButton(QStringLiteral("Measure Line"), annotationGroup);
        m_clearMeasurementLinesButton = new QPushButton(QStringLiteral("Clear"), annotationGroup);
        annotationButtons->addWidget(m_drawMeasurementLineButton);
        annotationButtons->addWidget(m_clearMeasurementLinesButton);
        annotationLayout->addLayout(annotationButtons);
        m_measurementInfoLabel = new QLabel(annotationGroup);
        m_measurementInfoLabel->hide();
        annotationLayout->addWidget(m_measurementInfoLabel);
        contentLayout->addWidget(annotationGroup);

        m_crossSectionGroup = new QGroupBox(QStringLiteral("Cross Section"), contentContainer);
        auto* crossSectionLayout = new QVBoxLayout(m_crossSectionGroup);
        auto* crossSectionButtons = new QHBoxLayout();
        m_drawCrossSectionButton = new QPushButton(QStringLiteral("Intensity Profile"), m_crossSectionGroup);
        m_clearCrossSectionButton = new QPushButton(QStringLiteral("Clear Profile"), m_crossSectionGroup);
        crossSectionButtons->addWidget(m_drawCrossSectionButton);
        crossSectionButtons->addWidget(m_clearCrossSectionButton);
        crossSectionButtons->addStretch();
        crossSectionLayout->addLayout(crossSectionButtons);
        m_crossSectionWidget = new InspectCrossSectionWidget(m_crossSectionGroup);
        crossSectionLayout->addWidget(m_crossSectionWidget);
        contentLayout->addWidget(m_crossSectionGroup);

        contentLayout->addStretch();

        connect(m_drawCrossSectionButton, &QPushButton::clicked, this, [this]()
        {
            if (currentLayerKey().isEmpty())
            {
                return;
            }
            m_crossSectionLayerKey = currentLayerKey();
            emit requestDrawCrossSectionLayer(currentLayerKey());
        });
        connect(m_clearCrossSectionButton, &QPushButton::clicked, this, [this]()
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        });
        connect(m_drawMeasurementLineButton, &QPushButton::clicked, this, [this]()
        {
            emit requestDrawMeasurementLine(currentLayerKey());
        });
        connect(m_clearMeasurementLinesButton, &QPushButton::clicked, this, [this]()
        {
            emit requestClearMeasurementLines(
                m_measurementLayerKey.isEmpty() ? currentLayerKey() : m_measurementLayerKey);
        });

        scrollArea->setWidget(contentContainer);
        mainLayout->addWidget(scrollArea);
    }

    // Create statistics controls for one layer
    QWidget* InspectWidget::createLayerInfoGroup(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        auto* group = new QGroupBox(inspectLayerTitle(normalizedLayerKey, false), this);
        auto* layout = new QVBoxLayout(group);
        LayerInfoGroup infoGroup;
        infoGroup.layerKey = normalizedLayerKey;
        infoGroup.groupBox = group;

        auto* slidersLayout = new QHBoxLayout();

        auto* minLabel = new QLabel(QStringLiteral("Min:"), group);
        auto* minSlider = new QSlider(Qt::Horizontal, group);
        minSlider->setRange(0, 255);
        minSlider->setValue(0);
        minSlider->setMinimumWidth(100);
        auto* minSliderValueLabel = new QLabel(QStringLiteral("0"), group);
        minSliderValueLabel->setMinimumWidth(50);
        minSliderValueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto* maxLabel = new QLabel(QStringLiteral("Max:"), group);
        auto* maxSlider = new QSlider(Qt::Horizontal, group);
        maxSlider->setRange(0, 255);
        maxSlider->setValue(255);
        maxSlider->setMinimumWidth(100);
        auto* maxSliderValueLabel = new QLabel(QStringLiteral("255"), group);
        maxSliderValueLabel->setMinimumWidth(50);
        maxSliderValueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        slidersLayout->addWidget(minLabel);
        slidersLayout->addWidget(minSlider, 1);
        slidersLayout->addWidget(minSliderValueLabel);
        slidersLayout->addWidget(maxLabel);
        slidersLayout->addWidget(maxSlider, 1);
        slidersLayout->addWidget(maxSliderValueLabel);
        layout->addLayout(slidersLayout);

        infoGroup.minSlider = minSlider;
        infoGroup.maxSlider = maxSlider;
        infoGroup.minSliderValueLabel = minSliderValueLabel;
        infoGroup.maxSliderValueLabel = maxSliderValueLabel;
        layout->addWidget(createStatisticsGroup(infoGroup));
        m_layerInfoGroups.insert(normalizedLayerKey, infoGroup);

        connect(minSlider, &QSlider::valueChanged, this,
                [this, normalizedLayerKey, minSlider, maxSlider, minSliderValueLabel](int value)
                {
                    if (value >= maxSlider->value())
                    {
                        QSignalBlocker blocker(minSlider);
                        minSlider->setValue(maxSlider->value() - 1);
                        value = maxSlider->value() - 1;
                    }
                    minSliderValueLabel->setText(QString::number(value));
                    onLayerSliderChanged(normalizedLayerKey, value, maxSlider->value());
                });
        connect(maxSlider, &QSlider::valueChanged, this,
                [this, normalizedLayerKey, minSlider, maxSlider, maxSliderValueLabel](int value)
                {
                    if (value <= minSlider->value())
                    {
                        QSignalBlocker blocker(maxSlider);
                        maxSlider->setValue(minSlider->value() + 1);
                        value = minSlider->value() + 1;
                    }
                    maxSliderValueLabel->setText(QString::number(value));
                    onLayerSliderChanged(normalizedLayerKey, minSlider->value(), value);
                });

        return group;
    }

    // Create labels for per layer image statistics
    QWidget* InspectWidget::createStatisticsGroup(LayerInfoGroup& infoGroup)
    {
        auto* group = new QGroupBox(QStringLiteral("Image Statistics"), this);
        auto* layout = new QGridLayout(group);

        layout->addWidget(new QLabel(QStringLiteral("Mean:"), group), 0, 0);
        auto* meanLabel = new QLabel(QStringLiteral("0.0"), group);
        layout->addWidget(meanLabel, 0, 1);

        layout->addWidget(new QLabel(QStringLiteral("Min:"), group), 0, 2);
        auto* minLabel = new QLabel(QStringLiteral("0"), group);
        layout->addWidget(minLabel, 0, 3);

        layout->addWidget(new QLabel(QStringLiteral("Max:"), group), 1, 0);
        auto* maxLabel = new QLabel(QStringLiteral("0"), group);
        layout->addWidget(maxLabel, 1, 1);

        layout->addWidget(new QLabel(QStringLiteral("Std Dev:"), group), 1, 2);
        auto* stdDevLabel = new QLabel(QStringLiteral("0.0"), group);
        layout->addWidget(stdDevLabel, 1, 3);

        layout->addWidget(new QLabel(QStringLiteral("Pixels:"), group), 2, 0);
        auto* pixelCountLabel = new QLabel(QStringLiteral("0"), group);
        layout->addWidget(pixelCountLabel, 2, 1, 1, 3);

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
        LayerInfoGroup& infoGroup = it.value();

        LayerInspectState& state = getOrCreateLayerState(normalizedLayerKey);
        state.stats = stats;
        state.hasStats = stats.hasData();
        if (!state.hasStats)
        {
            return;
        }

        scopeone::core::DocumentLayer layer;
        if (!m_sceneModel || !m_sceneModel->findLayer(normalizedLayerKey, layer))
        {
            return;
        }

        const int maxValue = qMax(1, layer.display.levelDomainMax);
        const int displayMin = qBound(0, layer.display.levelMin, maxValue - 1);
        const int displayMax = qBound(displayMin + 1, layer.display.levelMax, maxValue);
        infoGroup.minSlider->setRange(0, maxValue);
        infoGroup.maxSlider->setRange(0, maxValue);
        {
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(displayMin);
            infoGroup.maxSlider->setValue(displayMax);
        }
        infoGroup.minSliderValueLabel->setText(QString::number(displayMin));
        infoGroup.maxSliderValueLabel->setText(QString::number(displayMax));
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

        infoGroup.pixelCountLabel->setText(QString::number(stats.totalPixels));
    }

    // Enable controls according to live camera and selected layer state
    void InspectWidget::updateControlsState()
    {
        const QString layerKey = currentLayerKey();
        const auto currentState = m_layerStates.constFind(layerKey);
        const bool currentLayerHasStats = currentState != m_layerStates.constEnd() && currentState.value().hasStats;
        const bool liveCrossSectionEnabled = m_cameraInitialized
                                             && isLiveLayerKey(layerKey)
                                             && m_availableCameraIds.contains(currentLayerCameraId());
        const bool toolCrossSectionEnabled = scopeone::core::ScopeOneCore::isToolLayerKey(layerKey)
                                             && currentLayerHasStats;
        const bool staticCrossSectionEnabled = scopeone::core::ScopeOneCore::isStaticLayerKey(layerKey)
                                               && currentLayerHasStats;
        const bool crossSectionEnabled = !layerKey.isEmpty()
                                         && (liveCrossSectionEnabled
                                             || toolCrossSectionEnabled
                                             || staticCrossSectionEnabled);
        m_drawCrossSectionButton->setEnabled(crossSectionEnabled);
        m_clearCrossSectionButton->setEnabled(m_cameraInitialized || !layerKey.isEmpty());
        const bool annotationEnabled = !layerKey.isEmpty()
                                       && m_availableLayerKeys.contains(layerKey);
        m_drawMeasurementLineButton->setEnabled(annotationEnabled);
        m_clearMeasurementLinesButton->setEnabled(annotationEnabled);

        for (auto it = m_layerInfoGroups.begin(); it != m_layerInfoGroups.end(); ++it)
        {
            LayerInfoGroup& infoGroup = it.value();
            const auto stateIt = m_layerStates.constFind(infoGroup.layerKey);
            const bool hasStats = stateIt != m_layerStates.constEnd() && stateIt.value().hasStats;
            const bool isActiveLayer = infoGroup.layerKey == layerKey;
            infoGroup.minSlider->setEnabled(hasStats && isActiveLayer);
            infoGroup.maxSlider->setEnabled(hasStats && isActiveLayer);
        }
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

    // Apply manual display range changes from layer sliders
    void InspectWidget::onLayerSliderChanged(const QString& layerKey, int minValue, int maxValue)
    {
        auto stateIt = m_layerStates.find(layerKey);
        if (stateIt == m_layerStates.end())
        {
            return;
        }
        const LayerInspectState& state = stateIt.value();
        if (!state.hasStats)
        {
            return;
        }
        m_workspace->setLayerAutoStretchEnabled(layerKey, false);
        m_sceneModel->setLayerDisplayLevels(
            layerKey, minValue, maxValue, qMax(1, state.stats.maxValue));
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
