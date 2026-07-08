#include "InspectWidget.h"

#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QFont>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QtMath>
#include <QtGlobal>

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
            return scopeone::core::ScopeOneCore::isProcessedLayerKey(layerKey)
                       ? QStringLiteral("proc")
                       : QStringLiteral("raw");
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

    protected:
        // Paint the cross section curve and its summary labels
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), QColor(24, 24, 24));
            painter.setRenderHint(QPainter::Antialiasing, true);

            if (m_values.isEmpty())
            {
                painter.setPen(QColor(150, 150, 150));
                painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No cross section"));
                return;
            }

            const QRect plotRect = rect().adjusted(40, 24, -12, -28);
            painter.setPen(QColor(100, 100, 100));
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

            painter.setPen(QColor(220, 220, 220));
            painter.drawText(QRect(8, 4, width() - 16, 16),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1  N=%2  Min=%3  Max=%4")
                             .arg(m_title)
                             .arg(m_values.size())
                             .arg(minValue)
                             .arg(maxValue));

            painter.setPen(QColor(180, 180, 180));
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

            painter.setPen(QPen(QColor(255, 200, 0), 1.5));
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

    struct LayerHistogramData
    {
        QString layerKey;
        scopeone::core::ScopeOneCore::HistogramStats stats;
        QColor color{Qt::blue};
    };

    class InspectHistogramWidget : public QWidget
    {
    public:
        explicit InspectHistogramWidget(QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setMinimumHeight(150);
        }

        // Store histogram data for one layer and repaint
        void updateLayerHistogram(const QString& layerKey,
                                  const scopeone::core::ScopeOneCore::HistogramStats& stats,
                                  const QColor& color)
        {
            LayerHistogramData data;
            data.layerKey = layerKey;
            data.stats = stats;
            data.color = color;
            m_layerData[layerKey] = data;
            update();
        }

        // Toggle logarithmic histogram display
        void setLogScale(bool logScale)
        {
            m_logScale = logScale;
            update();
        }

    protected:
        // Paint all tracked camera histograms in one chart
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);

            const QRect rect = this->rect().adjusted(30, 10, -10, -20);

            painter.fillRect(rect, QColor(240, 240, 240));
            painter.setPen(QPen(Qt::black, 1));
            painter.drawRect(rect);

            if (m_layerData.isEmpty())
            {
                painter.drawText(rect, Qt::AlignCenter, QStringLiteral("No Layer Data"));
                return;
            }

            int globalMaxValue = 255;
            int globalMaxCount = 0;
            for (const LayerHistogramData& layerData : m_layerData)
            {
                if (!layerData.stats.hasData() || layerData.stats.histogram.empty())
                {
                    continue;
                }
                globalMaxValue = qMax(globalMaxValue, layerData.stats.maxValue);
                for (int count : layerData.stats.histogram)
                {
                    globalMaxCount = qMax(globalMaxCount, count);
                }
            }

            if (globalMaxCount == 0)
            {
                painter.drawText(rect, Qt::AlignCenter, QStringLiteral("No Histogram Data"));
                return;
            }

            for (const LayerHistogramData& layerData : m_layerData)
            {
                if (!layerData.stats.hasData() || layerData.stats.histogram.empty())
                {
                    continue;
                }

                const int histSize = static_cast<int>(layerData.stats.histogram.size());
                QColor histColor = layerData.color;
                histColor.setAlpha(180);
                painter.setPen(QPen(histColor, 1));

                for (int i = 0; i < histSize; ++i)
                {
                    const int x = rect.left() + (i * rect.width()) / histSize;
                    const int count = layerData.stats.histogram[static_cast<size_t>(i)];

                    double normalizedCount = 0.0;
                    if (m_logScale && count > 0)
                    {
                        normalizedCount = log10(count + 1.0) / log10(globalMaxCount + 1.0);
                    }
                    else
                    {
                        normalizedCount = static_cast<double>(count) / globalMaxCount;
                    }

                    const int height = static_cast<int>(normalizedCount * rect.height());
                    if (height > 0)
                    {
                        painter.drawLine(x, rect.bottom(), x, rect.bottom() - height);
                    }
                }
            }

            drawAxes(painter, rect, globalMaxValue);
        }

    private:
        // Draw intensity and count axes for the histogram plot
        void drawAxes(QPainter& painter, const QRect& rect, int maxValue)
        {
            painter.setPen(QPen(Qt::black, 1));

            QList<int> xTicks;
            xTicks << 0 << maxValue / 4 << maxValue / 2 << (maxValue * 3) / 4 << maxValue;

            for (int i = 0; i < xTicks.size(); ++i)
            {
                const int x = rect.left() + (i * rect.width()) / (xTicks.size() - 1);
                painter.drawLine(x, rect.bottom(), x, rect.bottom() + 5);

                const QString label = QString::number(xTicks[i]);
                const QRect textRect(x - 25, rect.bottom() + 5, 50, 20);
                painter.drawText(textRect, Qt::AlignCenter, label);
            }

            painter.drawLine(rect.left(), rect.top(), rect.left(), rect.bottom());

            int maxCount = 0;
            for (const LayerHistogramData& layerData : m_layerData)
            {
                if (!layerData.stats.hasData())
                {
                    continue;
                }
                for (int count : layerData.stats.histogram)
                {
                    maxCount = qMax(maxCount, count);
                }
            }

            if (maxCount <= 0)
            {
                return;
            }

            QList<int> yTicks;
            if (m_logScale)
            {
                yTicks = {1, 10, 100, 1000, 10000};
            }
            else
            {
                int step = maxCount / 4;
                if (step == 0)
                {
                    step = 1;
                }

                int magnitude = 1;
                while (step > magnitude * 10)
                {
                    magnitude *= 10;
                }
                step = ((step / magnitude) + 1) * magnitude;

                for (int i = 0; i <= 4; ++i)
                {
                    const int value = i * step;
                    if (value <= maxCount)
                    {
                        yTicks.append(value);
                    }
                }
            }

            for (int count : yTicks)
            {
                if (count > maxCount)
                {
                    continue;
                }

                double normalizedCount = 0.0;
                if (m_logScale && count > 0)
                {
                    normalizedCount = log10(count + 1.0) / log10(maxCount + 1.0);
                }
                else
                {
                    normalizedCount = static_cast<double>(count) / maxCount;
                }

                const int y = rect.bottom() - static_cast<int>(normalizedCount * rect.height());
                painter.drawLine(rect.left() - 5, y, rect.left(), y);

                QString label;
                if (count >= 1000)
                {
                    label = QStringLiteral("%1k").arg(count / 1000.0, 0, 'f', 1);
                }
                else
                {
                    label = QString::number(count);
                }
                const QRect textRect(0, y - 10, 25, 20);
                painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, label);
            }
        }

        QHash<QString, LayerHistogramData> m_layerData;
        bool m_logScale{false};
    };

    // Create the inspection panel and subscribe to core analysis signals
    InspectWidget::InspectWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("InspectWidget requires ScopeOneCore");
        }

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

    // Track the currently selected preview layer
    void InspectWidget::setCurrentLayer(const QString& layerKey)
    {
        m_currentLayerKey = layerKey.trimmed();
        if (!m_crossSectionLayerKey.isEmpty() && m_crossSectionLayerKey != m_currentLayerKey)
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        }
        updateLayerVisibility();
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

        if (!m_crossSectionLayerKey.isEmpty() && !m_availableLayerKeys.contains(m_crossSectionLayerKey))
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        }

        if (!m_currentLayerKey.isEmpty() && !m_availableLayerKeys.contains(m_currentLayerKey))
        {
            m_currentLayerKey.clear();
            clearCrossSectionProfile();
        }
        updateLayerVisibility();
        updateControlsState();
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

        if (!m_currentLayerKey.isEmpty()
            && isLiveLayerKey(m_currentLayerKey)
            && !m_availableCameraIds.contains(currentLayerCameraId()))
        {
            m_currentLayerKey.clear();
            clearCrossSectionProfile();
        }
        updateLayerVisibility();
        updateControlsState();
    }

    void InspectWidget::setCrossSectionVisible(bool visible)
    {
        m_crossSectionGroup->setVisible(visible);
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

    // Clear all layer inspect groups
    void InspectWidget::clearInspect()
    {
        setAvailableLayers({});
        setAvailableCameras({});
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
        if (m_currentLayerKey == trimmedLayerKey)
        {
            clearCrossSectionProfile();
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

    // Display a freshly computed cross section profile for one layer
    void InspectWidget::setLayerCrossSectionProfile(const QString& layerKey, const QVector<int>& values)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty() || trimmedLayerKey != m_currentLayerKey)
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

        m_crossSectionGroup = new QGroupBox(QStringLiteral("Cross Section"), contentContainer);
        auto* crossSectionLayout = new QVBoxLayout(m_crossSectionGroup);
        auto* crossSectionButtons = new QHBoxLayout();
        m_drawCrossSectionButton = new QPushButton(QStringLiteral("Draw Cross Section"), m_crossSectionGroup);
        m_clearCrossSectionButton = new QPushButton(QStringLiteral("Clear Cross Section"), m_crossSectionGroup);
        crossSectionButtons->addWidget(m_drawCrossSectionButton);
        crossSectionButtons->addWidget(m_clearCrossSectionButton);
        crossSectionButtons->addStretch();
        crossSectionLayout->addLayout(crossSectionButtons);
        m_crossSectionWidget = new InspectCrossSectionWidget(m_crossSectionGroup);
        crossSectionLayout->addWidget(m_crossSectionWidget);
        contentLayout->addWidget(m_crossSectionGroup);

        m_histogramContainerLayout = new QVBoxLayout();
        m_histogramContainerLayout->setSpacing(10);
        contentLayout->addLayout(m_histogramContainerLayout);
        contentLayout->addStretch();

        connect(m_drawCrossSectionButton, &QPushButton::clicked, this, [this]()
        {
            if (m_currentLayerKey.isEmpty())
            {
                return;
            }
            m_crossSectionLayerKey = m_currentLayerKey;
            emit requestDrawCrossSectionLayer(m_currentLayerKey);
        });
        connect(m_clearCrossSectionButton, &QPushButton::clicked, this, [this]()
        {
            clearCrossSectionProfile();
            emit requestClearCrossSection();
        });

        scrollArea->setWidget(contentContainer);
        mainLayout->addWidget(scrollArea);
    }

    // Create histogram controls for one layer
    QWidget* InspectWidget::createLayerInfoGroup(const QString& layerKey)
    {
        const QString normalizedLayerKey = layerKey.trimmed();
        const QString cameraId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(normalizedLayerKey);
        const bool processed = scopeone::core::ScopeOneCore::isProcessedLayerKey(normalizedLayerKey);
        auto* group = new QGroupBox(
            QStringLiteral("Layer - %1 [%2]").arg(cameraId, inspectLayerSourceLabel(normalizedLayerKey)),
            this);
        auto* layout = new QVBoxLayout(group);
        LayerInfoGroup infoGroup;
        infoGroup.layerKey = normalizedLayerKey;
        infoGroup.cameraId = cameraId;
        infoGroup.processed = processed;
        infoGroup.groupBox = group;

        auto* histLabel = new QLabel(QStringLiteral("Histogram"), group);
        QFont boldFont = histLabel->font();
        boldFont.setBold(true);
        histLabel->setFont(boldFont);
        layout->addWidget(histLabel);

        auto* histogramWidget = new InspectHistogramWidget(group);
        layout->addWidget(histogramWidget);
        infoGroup.histogramWidget = histogramWidget;

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

        auto* histControlLayout = new QHBoxLayout();
        auto* autoButton = new QPushButton(QStringLiteral("Auto"), group);
        auto* fullButton = new QPushButton(QStringLiteral("Full"), group);
        auto* autoStretchCheckBox = new QCheckBox(QStringLiteral("Auto-stretch"), group);
        auto* logScaleCheckBox = new QCheckBox(QStringLiteral("Log hist"), group);
        histControlLayout->addWidget(autoButton);
        histControlLayout->addWidget(fullButton);
        histControlLayout->addWidget(autoStretchCheckBox);
        histControlLayout->addWidget(logScaleCheckBox);
        histControlLayout->addStretch();
        layout->addLayout(histControlLayout);

        infoGroup.autoButton = autoButton;
        infoGroup.fullButton = fullButton;
        infoGroup.autoStretchCheckBox = autoStretchCheckBox;
        infoGroup.logScaleCheckBox = logScaleCheckBox;
        infoGroup.minSlider = minSlider;
        infoGroup.maxSlider = maxSlider;
        infoGroup.minSliderValueLabel = minSliderValueLabel;
        infoGroup.maxSliderValueLabel = maxSliderValueLabel;
        layout->addWidget(createStatisticsGroup(infoGroup));
        m_layerInfoGroups.insert(normalizedLayerKey, infoGroup);

        connect(autoButton, &QPushButton::clicked, this, [this, normalizedLayerKey]()
        {
            onAutoButtonClicked(normalizedLayerKey);
        });
        connect(fullButton, &QPushButton::clicked, this, [this, normalizedLayerKey]()
        {
            onFullButtonClicked(normalizedLayerKey);
        });
        connect(autoStretchCheckBox, &QCheckBox::toggled, this, [this, normalizedLayerKey](bool checked)
        {
            onAutoStretchChanged(normalizedLayerKey, checked);
        });
        connect(logScaleCheckBox, &QCheckBox::toggled, this, [this, normalizedLayerKey](bool checked)
        {
            onLogScaleChanged(normalizedLayerKey, checked);
        });
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

        QWidget* histogramGroup = createLayerInfoGroup(normalizedLayerKey);
        m_histogramContainerLayout->addWidget(histogramGroup);
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
        m_histogramContainerLayout->removeWidget(infoGroup.groupBox);
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

        const QString cameraId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(normalizedLayerKey);
        const bool processed = scopeone::core::ScopeOneCore::isProcessedLayerKey(normalizedLayerKey);
        LayerInspectState& state = getOrCreateLayerState(normalizedLayerKey, cameraId, processed);
        state.stats = stats;
        state.hasStats = stats.hasData();
        if (state.hasStats && !state.displayRangeValid)
        {
            state.maxDisplayValue = stats.maxValue > 0 ? stats.maxValue : 255;
            state.displayMin = 0;
            state.displayMax = state.maxDisplayValue;
            state.displayRangeValid = true;
        }

        if (state.hasStats && state.autoStretchEnabled)
        {
            applyAutoStretch(state);
        }
        else if (state.hasStats)
        {
            state.maxDisplayValue = stats.maxValue > 0 ? stats.maxValue : 255;
        }

        if (!state.hasStats)
        {
            return;
        }

        const QColor layerColor = getLayerColor(normalizedLayerKey);
        infoGroup.histogramWidget->updateLayerHistogram(normalizedLayerKey, stats, layerColor);

        const int maxValue = state.maxDisplayValue > 0 ? state.maxDisplayValue : 255;
        infoGroup.minSlider->setRange(0, maxValue);
        infoGroup.maxSlider->setRange(0, maxValue);
        {
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(state.displayMin);
            infoGroup.maxSlider->setValue(state.displayMax);
        }
        infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
        infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));

        updateStatisticsDisplay(normalizedLayerKey, stats);
        updateControlsState();
    }

    // Apply the computed auto display range once
    void InspectWidget::onAutoButtonClicked(const QString& layerKey)
    {
        auto stateIt = m_layerStates.find(layerKey);
        if (stateIt == m_layerStates.end())
        {
            return;
        }
        LayerInspectState& state = stateIt.value();
        if (!state.hasStats)
        {
            return;
        }

        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }
        LayerInfoGroup& infoGroup = it.value();

        state.displayMin = state.stats.autoMinLevel;
        state.displayMax = state.stats.autoMaxLevel;
        state.maxDisplayValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
        state.displayRangeValid = true;

        {
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(state.displayMin);
            infoGroup.maxSlider->setValue(state.displayMax);
        }
        infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
        infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));

        emit displayRangeChanged(state.layerKey,
                                 state.displayMin,
                                 state.displayMax,
                                 state.maxDisplayValue);
    }

    // Expand the display range to the full pixel range
    void InspectWidget::onFullButtonClicked(const QString& layerKey)
    {
        auto stateIt = m_layerStates.find(layerKey);
        if (stateIt == m_layerStates.end())
        {
            return;
        }
        LayerInspectState& state = stateIt.value();
        if (!state.hasStats)
        {
            return;
        }

        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }
        LayerInfoGroup& infoGroup = it.value();

        const int maxValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
        state.displayMin = 0;
        state.displayMax = maxValue;
        state.maxDisplayValue = maxValue;
        state.displayRangeValid = true;

        {
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(0);
            infoGroup.maxSlider->setValue(maxValue);
        }
        infoGroup.minSliderValueLabel->setText(QString::number(0));
        infoGroup.maxSliderValueLabel->setText(QString::number(maxValue));

        onLayerSliderChanged(layerKey, 0, maxValue);
    }

    // Toggle continuous auto stretch for one layer
    void InspectWidget::onAutoStretchChanged(const QString& layerKey, bool checked)
    {
        auto stateIt = m_layerStates.find(layerKey);
        if (stateIt == m_layerStates.end())
        {
            return;
        }
        LayerInspectState& state = stateIt.value();
        state.autoStretchEnabled = checked;

        if (!checked || !state.hasStats)
        {
            return;
        }

        applyAutoStretch(state);

        auto it = m_layerInfoGroups.find(layerKey);
        if (it != m_layerInfoGroups.end())
        {
            LayerInfoGroup& infoGroup = it.value();
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(state.displayMin);
            infoGroup.maxSlider->setValue(state.displayMax);
            infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
            infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));
        }

        emit displayRangeChanged(state.layerKey,
                                 state.displayMin,
                                 state.displayMax,
                                 state.maxDisplayValue);
    }

    // Toggle logarithmic histogram scaling for one layer
    void InspectWidget::onLogScaleChanged(const QString& layerKey, bool checked)
    {
        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }
        it.value().histogramWidget->setLogScale(checked);
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
        const auto currentState = m_layerStates.constFind(m_currentLayerKey);
        const bool currentLayerHasStats = currentState != m_layerStates.constEnd() && currentState.value().hasStats;
        const bool liveCrossSectionEnabled = m_cameraInitialized
                                             && isLiveLayerKey(m_currentLayerKey)
                                             && m_availableCameraIds.contains(currentLayerCameraId());
        const bool staticCrossSectionEnabled = scopeone::core::ScopeOneCore::isStaticLayerKey(m_currentLayerKey)
                                               && currentLayerHasStats;
        const bool crossSectionEnabled = !m_currentLayerKey.isEmpty()
                                         && (liveCrossSectionEnabled || staticCrossSectionEnabled);
        m_drawCrossSectionButton->setEnabled(crossSectionEnabled);
        m_clearCrossSectionButton->setEnabled(m_cameraInitialized || !m_currentLayerKey.isEmpty());

        for (auto it = m_layerInfoGroups.begin(); it != m_layerInfoGroups.end(); ++it)
        {
            LayerInfoGroup& infoGroup = it.value();
            const auto stateIt = m_layerStates.constFind(infoGroup.layerKey);
            const bool hasStats = stateIt != m_layerStates.constEnd() && stateIt.value().hasStats;
            infoGroup.autoButton->setEnabled(hasStats);
            infoGroup.fullButton->setEnabled(hasStats);
            infoGroup.autoStretchCheckBox->setEnabled(hasStats);
            infoGroup.logScaleCheckBox->setEnabled(hasStats);
        }
    }

    // Shows inspect controls for the selected preview layer
    void InspectWidget::updateLayerVisibility()
    {
        for (auto it = m_layerInfoGroups.begin(); it != m_layerInfoGroups.end(); ++it)
        {
            LayerInfoGroup& infoGroup = it.value();
            infoGroup.groupBox->setVisible(!m_currentLayerKey.isEmpty()
                                           && infoGroup.layerKey == m_currentLayerKey);
        }
    }

    // Push auto range back into sliders and preview
    void InspectWidget::applyAutoStretch(LayerInspectState& state)
    {
        if (!state.hasStats)
        {
            return;
        }

        const int minLevel = state.stats.autoMinLevel;
        const int maxLevel = state.stats.autoMaxLevel;
        const int maxDisplayValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
        if (state.displayRangeValid
            && state.displayMin == minLevel
            && state.displayMax == maxLevel
            && state.maxDisplayValue == maxDisplayValue)
        {
            return;
        }

        state.displayMin = minLevel;
        state.displayMax = maxLevel;
        state.maxDisplayValue = maxDisplayValue;
        state.displayRangeValid = true;

        auto it = m_layerInfoGroups.find(state.layerKey);
        if (it != m_layerInfoGroups.end())
        {
            LayerInfoGroup& infoGroup = it.value();
            QSignalBlocker minBlocker(infoGroup.minSlider);
            QSignalBlocker maxBlocker(infoGroup.maxSlider);
            infoGroup.minSlider->setValue(minLevel);
            infoGroup.maxSlider->setValue(maxLevel);
            infoGroup.minSliderValueLabel->setText(QString::number(minLevel));
            infoGroup.maxSliderValueLabel->setText(QString::number(maxLevel));
        }

        emit displayRangeChanged(state.layerKey,
                                 minLevel,
                                 maxLevel,
                                 state.maxDisplayValue);
    }

    // Return persistent inspect state for one preview layer
    InspectWidget::LayerInspectState& InspectWidget::getOrCreateLayerState(
        const QString& layerKey,
        const QString& cameraId,
        bool processed)
    {
        auto it = m_layerStates.find(layerKey);
        if (it == m_layerStates.end())
        {
            LayerInspectState state;
            state.layerKey = layerKey;
            state.cameraId = cameraId;
            state.processed = processed;
            it = m_layerStates.insert(layerKey, state);
        }
        return it.value();
    }

    // Pick a stable display color from the layer key
    QColor InspectWidget::getLayerColor(const QString& layerKey) const
    {
        static const QList<QColor> layerColors = {
            QColor(0, 120, 215),
            QColor(232, 17, 35),
            QColor(16, 124, 16),
            QColor(247, 99, 12)
        };

        const int index = qHash(layerKey) % layerColors.size();
        return layerColors[index];
    }

    // Apply manual display range changes from layer sliders
    void InspectWidget::onLayerSliderChanged(const QString& layerKey, int minValue, int maxValue)
    {
        auto stateIt = m_layerStates.find(layerKey);
        if (stateIt == m_layerStates.end())
        {
            return;
        }
        LayerInspectState& state = stateIt.value();
        state.displayMin = minValue;
        state.displayMax = maxValue;
        state.displayRangeValid = true;
        state.autoStretchEnabled = false;

        auto it = m_layerInfoGroups.find(layerKey);
        if (it == m_layerInfoGroups.end())
        {
            return;
        }
        LayerInfoGroup& infoGroup = it.value();

        if (infoGroup.autoStretchCheckBox)
        {
            QSignalBlocker blocker(infoGroup.autoStretchCheckBox);
            infoGroup.autoStretchCheckBox->setChecked(false);
        }

        emit displayRangeChanged(state.layerKey,
                                 minValue,
                                 maxValue,
                                 state.maxDisplayValue);
    }

    QString InspectWidget::currentLayerCameraId() const
    {
        return scopeone::core::ScopeOneCore::sourceIdFromLayerKey(m_currentLayerKey);
    }
} // namespace scopeone::ui
