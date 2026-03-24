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

namespace scopeone::ui {

namespace {

QString inspectStreamKey(const QString& cameraId, bool processed)
{
    return QStringLiteral("%1:%2")
        .arg(processed ? QStringLiteral("proc") : QStringLiteral("raw"),
             cameraId.trimmed());
}

QString inspectStreamLabel(bool processed)
{
    return processed ? QStringLiteral("proc") : QStringLiteral("raw");
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

    void clear()
    {
        m_title.clear();
        m_values.clear();
        update();
    }

    void setProfile(const QString& cameraId, bool processed, const QVector<int>& values)
    {
        m_title = QStringLiteral("%1 [%2]").arg(cameraId, processed ? QStringLiteral("proc") : QStringLiteral("raw"));
        m_values = values;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.fillRect(rect(), QColor(24, 24, 24));
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (m_values.isEmpty()) {
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
        for (int value : m_values) {
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
        painter.drawText(QRect(-plotRect.height() / 2, -20, plotRect.height(), 16),
                         Qt::AlignCenter | Qt::AlignVCenter,
                         QStringLiteral("Intensity"));
        painter.restore();

        QPolygonF line;
        line.reserve(m_values.size());
        const int pointCount = m_values.size();
        for (int i = 0; i < pointCount; ++i) {
            const double x = (pointCount == 1)
                ? plotRect.center().x()
                : plotRect.left() + (static_cast<double>(i) * plotRect.width()) / static_cast<double>(pointCount - 1);
            const double yNorm = static_cast<double>(m_values.at(i) - minValue) / static_cast<double>(valueRange);
            const double y = plotRect.bottom() - yNorm * plotRect.height();
            line << QPointF(x, y);
        }

        painter.setPen(QPen(QColor(255, 200, 0), 1.5));
        if (line.size() == 1) {
            painter.drawEllipse(line.first(), 2.0, 2.0);
        } else {
            painter.drawPolyline(line);
        }
    }

private:
    QString m_title;
    QVector<int> m_values;
};

struct CameraHistogramData {
    QString cameraId;
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

    void updateCameraHistogram(const QString& cameraId,
                               const scopeone::core::ScopeOneCore::HistogramStats& stats,
                               const QColor& color)
    {
        CameraHistogramData data;
        data.cameraId = cameraId;
        data.stats = stats;
        data.color = color;
        m_cameraData[cameraId] = data;
        update();
    }

    void setLogScale(bool logScale)
    {
        m_logScale = logScale;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect rect = this->rect().adjusted(30, 10, -10, -20);

        painter.fillRect(rect, QColor(240, 240, 240));
        painter.setPen(QPen(Qt::black, 1));
        painter.drawRect(rect);

        if (m_cameraData.isEmpty()) {
            painter.drawText(rect, Qt::AlignCenter, QStringLiteral("No Camera Data"));
            return;
        }

        int globalMaxValue = 255;
        int globalMaxCount = 0;
        for (const CameraHistogramData& camData : m_cameraData) {
            if (!camData.stats.hasData() || camData.stats.histogram.empty()) {
                continue;
            }
            globalMaxValue = qMax(globalMaxValue, camData.stats.maxValue);
            for (int count : camData.stats.histogram) {
                globalMaxCount = qMax(globalMaxCount, count);
            }
        }

        if (globalMaxCount == 0) {
            painter.drawText(rect, Qt::AlignCenter, QStringLiteral("No Histogram Data"));
            return;
        }

        for (const CameraHistogramData& camData : m_cameraData) {
            if (!camData.stats.hasData() || camData.stats.histogram.empty()) {
                continue;
            }

            const int histSize = static_cast<int>(camData.stats.histogram.size());
            QColor histColor = camData.color;
            histColor.setAlpha(180);
            painter.setPen(QPen(histColor, 1));

            for (int i = 0; i < histSize; ++i) {
                const int x = rect.left() + (i * rect.width()) / histSize;
                const int count = camData.stats.histogram[static_cast<size_t>(i)];

                double normalizedCount = 0.0;
                if (m_logScale && count > 0) {
                    normalizedCount = log10(count + 1.0) / log10(globalMaxCount + 1.0);
                } else {
                    normalizedCount = static_cast<double>(count) / globalMaxCount;
                }

                const int height = static_cast<int>(normalizedCount * rect.height());
                if (height > 0) {
                    painter.drawLine(x, rect.bottom(), x, rect.bottom() - height);
                }
            }
        }

        drawAxes(painter, rect, globalMaxValue);
    }

private:
    void drawAxes(QPainter& painter, const QRect& rect, int maxValue)
    {
        painter.setPen(QPen(Qt::black, 1));

        QList<int> xTicks;
        xTicks << 0 << maxValue / 4 << maxValue / 2 << (maxValue * 3) / 4 << maxValue;

        for (int i = 0; i < xTicks.size(); ++i) {
            const int x = rect.left() + (i * rect.width()) / (xTicks.size() - 1);
            painter.drawLine(x, rect.bottom(), x, rect.bottom() + 5);

            const QString label = QString::number(xTicks[i]);
            const QRect textRect(x - 25, rect.bottom() + 5, 50, 20);
            painter.drawText(textRect, Qt::AlignCenter, label);
        }

        painter.drawLine(rect.left(), rect.top(), rect.left(), rect.bottom());

        int maxCount = 0;
        for (const CameraHistogramData& camData : m_cameraData) {
            if (!camData.stats.hasData()) {
                continue;
            }
            for (int count : camData.stats.histogram) {
                maxCount = qMax(maxCount, count);
            }
        }

        if (maxCount <= 0) {
            return;
        }

        QList<int> yTicks;
        if (m_logScale) {
            yTicks = {1, 10, 100, 1000, 10000};
        } else {
            int step = maxCount / 4;
            if (step == 0) {
                step = 1;
            }

            int magnitude = 1;
            while (step > magnitude * 10) {
                magnitude *= 10;
            }
            step = ((step / magnitude) + 1) * magnitude;

            for (int i = 0; i <= 4; ++i) {
                const int value = i * step;
                if (value <= maxCount) {
                    yTicks.append(value);
                }
            }
        }

        for (int count : yTicks) {
            if (count > maxCount) {
                continue;
            }

            double normalizedCount = 0.0;
            if (m_logScale && count > 0) {
                normalizedCount = log10(count + 1.0) / log10(maxCount + 1.0);
            } else {
                normalizedCount = static_cast<double>(count) / maxCount;
            }

            const int y = rect.bottom() - static_cast<int>(normalizedCount * rect.height());
            painter.drawLine(rect.left() - 5, y, rect.left(), y);

            QString label;
            if (count >= 1000) {
                label = QStringLiteral("%1k").arg(count / 1000.0, 0, 'f', 1);
            } else {
                label = QString::number(count);
            }
            const QRect textRect(0, y - 10, 25, 20);
            painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, label);
        }
    }

    QHash<QString, CameraHistogramData> m_cameraData;
    bool m_logScale{false};
};

InspectWidget::InspectWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
    : QWidget(parent)
    , m_scopeonecore(core)
{
    setWindowTitle(QStringLiteral("Inspect"));
    setupUI();
    updateControlsState();

    if (m_scopeonecore) {
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::imageHistogramReady,
                this, &InspectWidget::onImageHistogramReady);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::lineProfileUpdated,
                this, &InspectWidget::setCrossSectionProfile);
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::lineProfileCleared,
                this, &InspectWidget::clearCrossSectionProfile);
    }
}

void InspectWidget::onCameraInitialized(bool initialized)
{
    m_cameraInitialized = initialized;
    updateControlsState();

    if (!initialized) {
        clearCrossSectionProfile();
    }
}

void InspectWidget::setCurrentTarget(const QString& target)
{
    m_currentTarget = target;
    updateControlsState();
}

void InspectWidget::setAvailableCameras(const QStringList& cameraIds)
{
    for (auto it = m_cameraStates.begin(); it != m_cameraStates.end();) {
        if (!cameraIds.contains(it.value().cameraId)) {
            it = m_cameraStates.erase(it);
        } else {
            ++it;
        }
    }

    QList<QString> keysToRemove;
    for (auto it = m_cameraInfoGroups.begin(); it != m_cameraInfoGroups.end(); ++it) {
        if (!cameraIds.contains(it.value().cameraId)) {
            keysToRemove.append(it.key());
        }
    }
    for (const QString& key : keysToRemove) {
        removeCameraInfo(key);
    }
}

void InspectWidget::setCrossSectionVisible(bool visible)
{
    if (m_crossSectionGroup) {
        m_crossSectionGroup->setVisible(visible);
    }
}

void InspectWidget::setFrameInspect(const QString& cameraId,
                                      const scopeone::core::ScopeOneCore::HistogramStats& stats)
{
    if (cameraId.isEmpty() || !stats.hasData()) {
        clearInspect();
        return;
    }

    setAvailableCameras({cameraId});
    const QString streamKey = inspectStreamKey(cameraId, false);
    if (!m_cameraInfoGroups.contains(streamKey)) {
        addCameraInfo(cameraId, false);
    }
    setCurrentTarget(cameraId);
    updateCameraInspect(cameraId, false, stats);
}

void InspectWidget::clearInspect()
{
    setAvailableCameras({});
}

void InspectWidget::clearCrossSectionProfile()
{
    if (!m_crossSectionWidget) {
        return;
    }
    m_crossSectionWidget->clear();
}

void InspectWidget::setCrossSectionProfile(const QString& cameraId, bool processed, const QVector<int>& values)
{
    if (!m_crossSectionWidget) {
        return;
    }
    m_crossSectionWidget->setProfile(cameraId, processed, values);
}

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

    connect(m_drawCrossSectionButton, &QPushButton::clicked, this, [this]() {
        if (isAllTarget(m_currentTarget)) {
            return;
        }
        emit requestDrawCrossSection(m_currentTarget);
    });
    connect(m_clearCrossSectionButton, &QPushButton::clicked, this, [this]() {
        clearCrossSectionProfile();
        emit requestClearCrossSection();
    });

    scrollArea->setWidget(contentContainer);
    mainLayout->addWidget(scrollArea);
}

QWidget* InspectWidget::createCameraInfoGroup(const QString& cameraId, bool processed)
{
    const QString streamKey = inspectStreamKey(cameraId, processed);
    auto* group = new QGroupBox(
        QStringLiteral("Camera - %1 [%2]").arg(cameraId, inspectStreamLabel(processed)),
        this);
    auto* layout = new QVBoxLayout(group);
    CameraInfoGroup infoGroup;
    infoGroup.streamKey = streamKey;
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
    m_cameraInfoGroups.insert(streamKey, infoGroup);

    connect(autoButton, &QPushButton::clicked, this, [this, streamKey]() {
        onAutoButtonClicked(streamKey);
    });
    connect(fullButton, &QPushButton::clicked, this, [this, streamKey]() {
        onFullButtonClicked(streamKey);
    });
    connect(autoStretchCheckBox, &QCheckBox::toggled, this, [this, streamKey](bool checked) {
        onAutoStretchChanged(streamKey, checked);
    });
    connect(logScaleCheckBox, &QCheckBox::toggled, this, [this, streamKey](bool checked) {
        onLogScaleChanged(streamKey, checked);
    });
    connect(minSlider, &QSlider::valueChanged, this, [this, streamKey, minSlider, maxSlider, minSliderValueLabel](int value) {
        if (value >= maxSlider->value()) {
            minSlider->blockSignals(true);
            minSlider->setValue(maxSlider->value() - 1);
            minSlider->blockSignals(false);
            value = maxSlider->value() - 1;
        }
        minSliderValueLabel->setText(QString::number(value));
        onCameraSliderChanged(streamKey, value, maxSlider->value());
    });
    connect(maxSlider, &QSlider::valueChanged, this, [this, streamKey, minSlider, maxSlider, maxSliderValueLabel](int value) {
        if (value <= minSlider->value()) {
            maxSlider->blockSignals(true);
            maxSlider->setValue(minSlider->value() + 1);
            maxSlider->blockSignals(false);
            value = minSlider->value() + 1;
        }
        maxSliderValueLabel->setText(QString::number(value));
        onCameraSliderChanged(streamKey, minSlider->value(), value);
    });

    return group;
}

QWidget* InspectWidget::createStatisticsGroup(CameraInfoGroup& infoGroup)
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

void InspectWidget::addCameraInfo(const QString& cameraId, bool processed)
{
    const QString streamKey = inspectStreamKey(cameraId, processed);
    if (m_cameraInfoGroups.contains(streamKey)) {
        return;
    }

    QWidget* histogramGroup = createCameraInfoGroup(cameraId, processed);
    if (m_histogramContainerLayout) {
        m_histogramContainerLayout->addWidget(histogramGroup);
    }
    updateControlsState();
}

void InspectWidget::removeCameraInfo(const QString& streamKey)
{
    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }

    CameraInfoGroup& infoGroup = it.value();
    if (infoGroup.groupBox && m_histogramContainerLayout) {
        m_histogramContainerLayout->removeWidget(infoGroup.groupBox);
        infoGroup.groupBox->deleteLater();
    }
    m_cameraInfoGroups.erase(it);
}

void InspectWidget::updateCameraInspect(
    const QString& cameraId,
    bool processed,
    const scopeone::core::ScopeOneCore::HistogramStats& stats)
{
    // Sync histogram controls with fresh stats
    const QString streamKey = inspectStreamKey(cameraId, processed);
    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    CameraInfoGroup& infoGroup = it.value();

    CameraDisplayState& state = getOrCreateCameraState(cameraId, processed);
    state.stats = stats;
    state.hasStats = stats.hasData();
    if (state.hasStats && !state.displayRangeValid) {
        state.maxDisplayValue = stats.maxValue > 0 ? stats.maxValue : 255;
        state.displayMin = 0;
        state.displayMax = state.maxDisplayValue;
        state.displayRangeValid = true;
    }

    if (state.hasStats && state.autoStretchEnabled) {
        applyAutoStretch(state, streamKey);
    } else if (state.hasStats) {
        state.maxDisplayValue = stats.maxValue > 0 ? stats.maxValue : 255;
    }

    if (!state.hasStats) {
        return;
    }

    const QColor cameraColor = getCameraColor(streamKey);
    infoGroup.histogramWidget->updateCameraHistogram(streamKey, stats, cameraColor);

    const int maxValue = state.maxDisplayValue > 0 ? state.maxDisplayValue : 255;
    infoGroup.minSlider->setRange(0, maxValue);
    infoGroup.maxSlider->setRange(0, maxValue);
    infoGroup.minSlider->blockSignals(true);
    infoGroup.maxSlider->blockSignals(true);
    infoGroup.minSlider->setValue(state.displayMin);
    infoGroup.maxSlider->setValue(state.displayMax);
    infoGroup.minSlider->blockSignals(false);
    infoGroup.maxSlider->blockSignals(false);
    infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
    infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));

    updateStatisticsDisplay(cameraId, processed, stats);
}

void InspectWidget::onImageHistogramReady(
    const QString& cameraId,
    bool processed,
    const scopeone::core::ScopeOneCore::HistogramStats& stats)
{
    // Create UI state on first histogram update
    if (cameraId.isEmpty()) {
        return;
    }
    const QString streamKey = inspectStreamKey(cameraId, processed);
    if (!m_cameraInfoGroups.contains(streamKey)) {
        addCameraInfo(cameraId, processed);
    }
    updateCameraInspect(cameraId, processed, stats);
}

void InspectWidget::onAutoButtonClicked(const QString& streamKey)
{
    auto stateIt = m_cameraStates.find(streamKey);
    if (stateIt == m_cameraStates.end()) {
        return;
    }
    CameraDisplayState& state = stateIt.value();
    if (!state.hasStats) {
        return;
    }

    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    CameraInfoGroup& infoGroup = it.value();

    state.displayMin = state.stats.autoMinLevel;
    state.displayMax = state.stats.autoMaxLevel;
    state.maxDisplayValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
    state.displayRangeValid = true;

    infoGroup.minSlider->blockSignals(true);
    infoGroup.maxSlider->blockSignals(true);
    infoGroup.minSlider->setValue(state.displayMin);
    infoGroup.maxSlider->setValue(state.displayMax);
    infoGroup.minSlider->blockSignals(false);
    infoGroup.maxSlider->blockSignals(false);
    infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
    infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));

    emit displayRangeChanged(state.cameraId,
                             state.processed,
                             state.displayMin,
                             state.displayMax,
                             state.maxDisplayValue);
}

void InspectWidget::onFullButtonClicked(const QString& streamKey)
{
    auto stateIt = m_cameraStates.find(streamKey);
    if (stateIt == m_cameraStates.end()) {
        return;
    }
    CameraDisplayState& state = stateIt.value();
    if (!state.hasStats) {
        return;
    }

    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    CameraInfoGroup& infoGroup = it.value();

    const int maxValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
    state.displayMin = 0;
    state.displayMax = maxValue;
    state.maxDisplayValue = maxValue;
    state.displayRangeValid = true;

    infoGroup.minSlider->blockSignals(true);
    infoGroup.maxSlider->blockSignals(true);
    infoGroup.minSlider->setValue(0);
    infoGroup.maxSlider->setValue(maxValue);
    infoGroup.minSlider->blockSignals(false);
    infoGroup.maxSlider->blockSignals(false);
    infoGroup.minSliderValueLabel->setText(QString::number(0));
    infoGroup.maxSliderValueLabel->setText(QString::number(maxValue));

    onCameraSliderChanged(streamKey, 0, maxValue);
}

void InspectWidget::onAutoStretchChanged(const QString& streamKey, bool checked)
{
    auto stateIt = m_cameraStates.find(streamKey);
    if (stateIt == m_cameraStates.end()) {
        return;
    }
    CameraDisplayState& state = stateIt.value();
    state.autoStretchEnabled = checked;

    if (!checked || !state.hasStats) {
        return;
    }

    applyAutoStretch(state, streamKey);

    auto it = m_cameraInfoGroups.find(streamKey);
    if (it != m_cameraInfoGroups.end()) {
        CameraInfoGroup& infoGroup = it.value();
        infoGroup.minSlider->blockSignals(true);
        infoGroup.maxSlider->blockSignals(true);
        infoGroup.minSlider->setValue(state.displayMin);
        infoGroup.maxSlider->setValue(state.displayMax);
        infoGroup.minSlider->blockSignals(false);
        infoGroup.maxSlider->blockSignals(false);
        infoGroup.minSliderValueLabel->setText(QString::number(state.displayMin));
        infoGroup.maxSliderValueLabel->setText(QString::number(state.displayMax));
    }

    emit displayRangeChanged(state.cameraId,
                             state.processed,
                             state.displayMin,
                             state.displayMax,
                             state.maxDisplayValue);
}

void InspectWidget::onLogScaleChanged(const QString& streamKey, bool checked)
{
    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    it.value().histogramWidget->setLogScale(checked);
}

void InspectWidget::updateStatisticsDisplay(
    const QString& cameraId,
    bool processed,
    const scopeone::core::ScopeOneCore::HistogramStats& stats)
{
    auto it = m_cameraInfoGroups.find(inspectStreamKey(cameraId, processed));
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    CameraInfoGroup& infoGroup = it.value();

    if (stats.bitDepth > 8) {
        infoGroup.meanLabel->setText(QString::number(stats.mean, 'f', 0));
        infoGroup.minLabel->setText(QString::number(static_cast<int>(stats.minVal)));
        infoGroup.maxLabel->setText(QString::number(static_cast<int>(stats.maxVal)));
        infoGroup.stdDevLabel->setText(QString::number(stats.stdDev, 'f', 0));
    } else {
        infoGroup.meanLabel->setText(QString::number(stats.mean, 'f', 1));
        infoGroup.minLabel->setText(QString::number(static_cast<int>(stats.minVal)));
        infoGroup.maxLabel->setText(QString::number(static_cast<int>(stats.maxVal)));
        infoGroup.stdDevLabel->setText(QString::number(stats.stdDev, 'f', 1));
    }

    infoGroup.pixelCountLabel->setText(QString::number(stats.totalPixels));
}

void InspectWidget::updateControlsState()
{
    // Keep cross section controls tied to target state
    const bool crossSectionEnabled = m_cameraInitialized && !isAllTarget(m_currentTarget);
    if (m_drawCrossSectionButton) {
        m_drawCrossSectionButton->setEnabled(crossSectionEnabled);
    }
    if (m_clearCrossSectionButton) {
        m_clearCrossSectionButton->setEnabled(m_cameraInitialized);
    }

    for (auto it = m_cameraInfoGroups.begin(); it != m_cameraInfoGroups.end(); ++it) {
        CameraInfoGroup& infoGroup = it.value();
        infoGroup.autoButton->setEnabled(m_cameraInitialized);
        infoGroup.fullButton->setEnabled(m_cameraInitialized);
        infoGroup.autoStretchCheckBox->setEnabled(m_cameraInitialized);
        infoGroup.logScaleCheckBox->setEnabled(m_cameraInitialized);
    }
}

void InspectWidget::applyAutoStretch(CameraDisplayState& state, const QString& streamKey)
{
    // Push auto range back into sliders and preview
    if (!state.hasStats) {
        return;
    }

    const int minLevel = state.stats.autoMinLevel;
    const int maxLevel = state.stats.autoMaxLevel;
    state.displayMin = minLevel;
    state.displayMax = maxLevel;
    state.maxDisplayValue = state.stats.maxValue > 0 ? state.stats.maxValue : 255;
    state.displayRangeValid = true;

    auto it = m_cameraInfoGroups.find(streamKey);
    if (it != m_cameraInfoGroups.end()) {
        CameraInfoGroup& infoGroup = it.value();
        infoGroup.minSlider->blockSignals(true);
        infoGroup.maxSlider->blockSignals(true);
        infoGroup.minSlider->setValue(minLevel);
        infoGroup.maxSlider->setValue(maxLevel);
        infoGroup.minSlider->blockSignals(false);
        infoGroup.maxSlider->blockSignals(false);
        infoGroup.minSliderValueLabel->setText(QString::number(minLevel));
        infoGroup.maxSliderValueLabel->setText(QString::number(maxLevel));
    }

    emit displayRangeChanged(state.cameraId,
                             state.processed,
                             minLevel,
                             maxLevel,
                             state.maxDisplayValue);
}

InspectWidget::CameraDisplayState& InspectWidget::getOrCreateCameraState(const QString& cameraId, bool processed)
{
    const QString streamKey = inspectStreamKey(cameraId, processed);
    auto it = m_cameraStates.find(streamKey);
    if (it == m_cameraStates.end()) {
        CameraDisplayState state;
        state.cameraId = cameraId;
        state.processed = processed;
        it = m_cameraStates.insert(streamKey, state);
    }
    return it.value();
}

QColor InspectWidget::getCameraColor(const QString& cameraId) const
{
    static const QList<QColor> cameraColors = {
        QColor(0, 120, 215),
        QColor(232, 17, 35),
        QColor(16, 124, 16),
        QColor(247, 99, 12)
    };

    const int index = qHash(cameraId) % cameraColors.size();
    return cameraColors[index];
}

void InspectWidget::onCameraSliderChanged(const QString& streamKey, int minValue, int maxValue)
{
    auto stateIt = m_cameraStates.find(streamKey);
    if (stateIt == m_cameraStates.end()) {
        return;
    }
    CameraDisplayState& state = stateIt.value();
    state.displayMin = minValue;
    state.displayMax = maxValue;
    state.displayRangeValid = true;
    state.autoStretchEnabled = false;

    auto it = m_cameraInfoGroups.find(streamKey);
    if (it == m_cameraInfoGroups.end()) {
        return;
    }
    CameraInfoGroup& infoGroup = it.value();

    if (infoGroup.autoStretchCheckBox) {
        QSignalBlocker blocker(infoGroup.autoStretchCheckBox);
        infoGroup.autoStretchCheckBox->setChecked(false);
    }

    emit displayRangeChanged(state.cameraId,
                             state.processed,
                             minValue,
                             maxValue,
                             state.maxDisplayValue);
}

bool InspectWidget::isAllTarget(const QString& target) const
{
    return target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0;
}

} // namespace scopeone::ui
