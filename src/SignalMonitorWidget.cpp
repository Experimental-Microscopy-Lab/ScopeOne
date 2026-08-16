#include "SignalMonitorWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace scopeone::ui
{
    namespace
    {
        QString formattedValue(double value, const QString& unit)
        {
            return QStringLiteral("%1 %2").arg(value, 0, 'g', 5).arg(unit);
        }

        QString settingKey(const QString& sourceId, const QString& parameterKey)
        {
            return QStringLiteral("signalSources/%1/%2").arg(sourceId, parameterKey);
        }
    }

    class SignalTracePlot final : public QWidget
    {
    public:
        explicit SignalTracePlot(QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setMinimumHeight(220);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        }

        double appendChunk(const scopeone::core::TimeSeriesChunk& chunk,
                           double windowSeconds)
        {
            m_windowSeconds = windowSeconds;
            for (int index = 0; index < chunk.values.size(); ++index)
            {
                m_times.append(chunk.startTimeSeconds
                               + (static_cast<double>(index) + 0.5)
                               * chunk.sampleIntervalSeconds);
                m_values.append(chunk.values[index]);
            }
            trim();
            update();
            return m_values.constLast();
        }

        void setWindowSeconds(double windowSeconds)
        {
            m_windowSeconds = windowSeconds;
            trim();
            update();
        }

        void clear()
        {
            m_times.clear();
            m_values.clear();
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), palette().color(QPalette::Base));

            const QRectF plotRect = QRectF(rect()).adjusted(62.0, 12.0, -12.0, -30.0);
            if (plotRect.width() <= 1.0 || plotRect.height() <= 1.0)
            {
                return;
            }
            painter.setPen(palette().color(QPalette::Mid));
            painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());
            painter.drawLine(plotRect.bottomLeft(), plotRect.topLeft());

            const QColor textColor = palette().color(QPalette::Text);
            if (m_times.isEmpty() || m_values.isEmpty())
            {
                painter.setPen(textColor);
                painter.drawText(plotRect, Qt::AlignCenter, tr("No signal data"));
                return;
            }

            const double endTime = m_times.constLast();
            const double startTime = std::max(0.0, endTime - m_windowSeconds);
            const double displayedDuration = std::max(
                endTime - startTime, (std::numeric_limits<double>::epsilon)());
            const auto first = std::lower_bound(m_times.cbegin(), m_times.cend(), startTime);
            const int firstIndex = static_cast<int>(std::distance(m_times.cbegin(), first));
            const int lastIndex = std::min(m_times.size(), m_values.size());
            if (firstIndex >= lastIndex)
            {
                return;
            }

            double minimum = (std::numeric_limits<double>::max)();
            double maximum = (std::numeric_limits<double>::lowest)();
            for (int index = firstIndex; index < lastIndex; ++index)
            {
                minimum = std::min(minimum, m_values[index]);
                maximum = std::max(maximum, m_values[index]);
            }
            const bool nonnegative = minimum >= 0.0;
            const bool nonpositive = maximum <= 0.0;
            if (nonnegative)
            {
                minimum = 0.0;
            }
            if (nonpositive)
            {
                maximum = 0.0;
            }
            if (minimum == maximum)
            {
                maximum = minimum + 1.0;
            }
            const double padding = (maximum - minimum) * 0.05;
            if (!nonnegative)
            {
                minimum -= padding;
            }
            if (!nonpositive)
            {
                maximum += padding;
            }

            const auto mapX = [&](double time)
            {
                return plotRect.left()
                    + (time - startTime) / displayedDuration * plotRect.width();
            };
            const auto mapY = [&](double value)
            {
                return plotRect.bottom()
                    - (value - minimum) / (maximum - minimum) * plotRect.height();
            };

            painter.setPen(textColor);
            painter.drawText(QRectF(0.0, plotRect.top() - 6.0,
                                    plotRect.left() - 6.0, 20.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(maximum, 'g', 5));
            painter.drawText(QRectF(0.0, plotRect.bottom() - 10.0,
                                    plotRect.left() - 6.0, 20.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(minimum, 'g', 5));
            painter.drawText(QRectF(plotRect.left(), plotRect.bottom() + 4.0,
                                    plotRect.width(), 20.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1 s").arg(startTime, 0, 'g', 4));
            painter.drawText(QRectF(plotRect.left(), plotRect.bottom() + 4.0,
                                    plotRect.width(), 20.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1 s").arg(endTime, 0, 'g', 4));

            painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
            const int visiblePoints = lastIndex - firstIndex;
            const int pixelColumns = std::max(1, static_cast<int>(plotRect.width()));
            if (visiblePoints <= pixelColumns * 2)
            {
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.moveTo(mapX(m_times[firstIndex]), mapY(m_values[firstIndex]));
                for (int index = firstIndex + 1; index < lastIndex; ++index)
                {
                    path.lineTo(mapX(m_times[index]), mapY(m_values[index]));
                }
                painter.drawPath(path);
                return;
            }

            QVector<double> minima(pixelColumns, maximum);
            QVector<double> maxima(pixelColumns, minimum);
            QVector<bool> populated(pixelColumns, false);
            // Preserve extrema when samples outnumber horizontal pixels
            for (int index = firstIndex; index < lastIndex; ++index)
            {
                const int column = std::clamp(
                    static_cast<int>((m_times[index] - startTime)
                                     / displayedDuration * pixelColumns),
                    0,
                    pixelColumns - 1);
                minima[column] = std::min(minima[column], m_values[index]);
                maxima[column] = std::max(maxima[column], m_values[index]);
                populated[column] = true;
            }
            for (int column = 0; column < pixelColumns; ++column)
            {
                if (populated[column])
                {
                    const double x = plotRect.left() + column;
                    painter.drawLine(QPointF(x, mapY(minima[column])),
                                     QPointF(x, mapY(maxima[column])));
                }
            }
        }

    private:
        void trim()
        {
            if (m_times.isEmpty())
            {
                return;
            }
            const double cutoff = m_times.constLast() - m_windowSeconds;
            const auto first = std::lower_bound(m_times.cbegin(), m_times.cend(), cutoff);
            const int removeCount = static_cast<int>(std::distance(m_times.cbegin(), first));
            if (removeCount > 0)
            {
                m_times.remove(0, removeCount);
                m_values.remove(0, removeCount);
            }
        }

        QVector<double> m_times;
        QVector<double> m_values;
        double m_windowSeconds{10.0};
    };

    SignalMonitorWidget::SignalMonitorWidget(scopeone::core::ScopeOneCore* core,
                                             QWidget* parent)
        : QWidget(parent)
          , m_core(core)
    {
        auto* layout = new QVBoxLayout(this);
        auto* sourceForm = new QFormLayout();
        m_sourceCombo = new QComboBox(this);
        sourceForm->addRow(tr("Source"), m_sourceCombo);
        layout->addLayout(sourceForm);

        m_sourceForm = new QFormLayout();
        layout->addLayout(m_sourceForm);

        auto* traceForm = new QFormLayout();
        m_sampleIntervalSpin = new QDoubleSpinBox(this);
        m_sampleIntervalSpin->setRange(0.001, 10000.0);
        m_sampleIntervalSpin->setDecimals(3);
        m_sampleIntervalSpin->setSuffix(tr(" ms"));
        m_sampleIntervalSpin->setValue(10.0);
        traceForm->addRow(tr("Interval"), m_sampleIntervalSpin);

        m_windowDurationSpin = new QDoubleSpinBox(this);
        m_windowDurationSpin->setRange(0.1, 60.0);
        m_windowDurationSpin->setDecimals(1);
        m_windowDurationSpin->setSuffix(tr(" s"));
        m_windowDurationSpin->setValue(10.0);
        traceForm->addRow(tr("Window"), m_windowDurationSpin);

        m_scanImageCheck = new QCheckBox(tr("Build scan image"), this);
        traceForm->addRow(tr("Scan"), m_scanImageCheck);
        m_scanWidthSpin = new QSpinBox(this);
        m_scanWidthSpin->setRange(1, 8192);
        m_scanWidthSpin->setValue(256);
        traceForm->addRow(tr("Width"), m_scanWidthSpin);
        m_scanHeightSpin = new QSpinBox(this);
        m_scanHeightSpin->setRange(1, 8192);
        m_scanHeightSpin->setValue(256);
        traceForm->addRow(tr("Height"), m_scanHeightSpin);
        const auto markerCombo = [this](quint32 value)
        {
            auto* combo = new QComboBox(this);
            combo->addItem(tr("Off"), 0U);
            combo->addItem(tr("Marker 1"), 1U);
            combo->addItem(tr("Marker 2"), 2U);
            combo->addItem(tr("Marker 3"), 4U);
            combo->addItem(tr("Marker 4"), 8U);
            combo->setCurrentIndex(combo->findData(value));
            return combo;
        };
        m_frameStartMarkerCombo = markerCombo(1);
        traceForm->addRow(tr("Frame start"), m_frameStartMarkerCombo);
        m_lineStartMarkerCombo = markerCombo(2);
        traceForm->addRow(tr("Line start"), m_lineStartMarkerCombo);
        m_lineEndMarkerCombo = markerCombo(0);
        traceForm->addRow(tr("Line end"), m_lineEndMarkerCombo);
        m_frameEndMarkerCombo = markerCombo(0);
        traceForm->addRow(tr("Frame end"), m_frameEndMarkerCombo);
        m_serpentineCheck = new QCheckBox(tr("Reverse alternate lines"), this);
        traceForm->addRow(tr("Scan order"), m_serpentineCheck);
        layout->addLayout(traceForm);

        auto* buttonLayout = new QHBoxLayout();
        m_startButton = new QPushButton(tr("Start"), this);
        m_startButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_stopButton = new QPushButton(tr("Stop"), this);
        m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        m_stopButton->setEnabled(false);
        buttonLayout->addWidget(m_startButton);
        buttonLayout->addWidget(m_stopButton);
        layout->addLayout(buttonLayout);

        m_valueLabel = new QLabel(tr("Value: -"), this);
        m_statusLabel = new QLabel(tr("No signal sources available"), this);
        m_statusLabel->setWordWrap(true);
        layout->addWidget(m_valueLabel);
        layout->addWidget(m_statusLabel);
        m_plot = new SignalTracePlot(this);
        layout->addWidget(m_plot, 1);

        connect(m_sourceCombo, &QComboBox::currentIndexChanged,
                this, &SignalMonitorWidget::rebuildSourceParameters);
        connect(m_startButton, &QPushButton::clicked,
                this, &SignalMonitorWidget::startAcquisition);
        connect(m_stopButton, &QPushButton::clicked, this, [this]()
        {
            m_core->stopSignalTrace(m_activeSourceId);
        });
        connect(m_windowDurationSpin, &QDoubleSpinBox::valueChanged,
                m_plot, &SignalTracePlot::setWindowSeconds);
        connect(m_scanImageCheck, &QCheckBox::toggled, this, [this](bool enabled)
        {
            const bool editable = enabled && m_scanImageCheck->isEnabled();
            m_scanWidthSpin->setEnabled(editable);
            m_scanHeightSpin->setEnabled(editable);
            m_frameStartMarkerCombo->setEnabled(editable);
            m_lineStartMarkerCombo->setEnabled(editable);
            m_lineEndMarkerCombo->setEnabled(editable);
            m_frameEndMarkerCombo->setEnabled(editable);
            m_serpentineCheck->setEnabled(editable);
        });
        connect(m_core, &scopeone::core::ScopeOneCore::signalTimeSeriesReady,
                this, &SignalMonitorWidget::handleTimeSeries);
        connect(m_core, &scopeone::core::ScopeOneCore::signalSourceStateChanged,
                this, &SignalMonitorWidget::handleStateChanged);
        connect(m_core, &scopeone::core::ScopeOneCore::signalSourceError,
                this, [](const QString&, const QString& message)
                {
                    qCritical().noquote() << message;
                });

        m_scanImageCheck->setChecked(false);
        m_scanWidthSpin->setEnabled(false);
        m_scanHeightSpin->setEnabled(false);
        m_frameStartMarkerCombo->setEnabled(false);
        m_lineStartMarkerCombo->setEnabled(false);
        m_lineEndMarkerCombo->setEnabled(false);
        m_frameEndMarkerCombo->setEnabled(false);
        m_serpentineCheck->setEnabled(false);

        refreshSources();
    }

    void SignalMonitorWidget::refreshSources()
    {
        const QString selectedId = m_sourceCombo->currentData().toString();
        const QList<scopeone::core::SignalSourceDescriptor> sources = m_core->signalSources();
        {
            const QSignalBlocker blocker(m_sourceCombo);
            m_sourceCombo->clear();
            for (const auto& source : sources)
            {
                m_sourceCombo->addItem(source.name, source.id);
                m_sourceCombo->setItemData(
                    m_sourceCombo->count() - 1,
                    QStringLiteral("%1, %2 [%3]")
                        .arg(source.provider, source.quantity, source.unit),
                    Qt::ToolTipRole);
            }
            const int selectedIndex = m_sourceCombo->findData(selectedId);
            if (selectedIndex >= 0)
            {
                m_sourceCombo->setCurrentIndex(selectedIndex);
            }
        }
        const bool available = !sources.isEmpty();
        if (available)
        {
            const QString sourceId = m_sourceCombo->currentData().toString();
            const auto state = m_core->signalSourceState(sourceId);
            const bool active = state == scopeone::core::SignalSourceState::Starting
                || state == scopeone::core::SignalSourceState::Running
                || state == scopeone::core::SignalSourceState::Stopping;
            m_startButton->setEnabled(!active);
            m_stopButton->setEnabled(state == scopeone::core::SignalSourceState::Starting
                                     || state == scopeone::core::SignalSourceState::Running);
            m_statusLabel->setText(m_core->signalSourceStateMessage(sourceId));
        }
        else
        {
            m_startButton->setEnabled(false);
            m_stopButton->setEnabled(false);
            m_statusLabel->setText(tr("No signal source plugins found"));
        }
        rebuildSourceParameters();
    }

    void SignalMonitorWidget::rebuildSourceParameters()
    {
        while (m_sourceForm->rowCount() > 0)
        {
            m_sourceForm->removeRow(0);
        }
        m_parameterEditors.clear();

        const auto descriptor = currentDescriptor();
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (const auto& parameter : descriptor.parameters)
        {
            const QVariant storedValue = settings.value(
                settingKey(descriptor.id, parameter.key), parameter.defaultValue);
            QWidget* editor = nullptr;
            if (parameter.type == scopeone::core::SignalParameterType::Integer)
            {
                auto* spin = new QSpinBox(this);
                if (parameter.hasRange)
                {
                    spin->setRange(static_cast<int>(parameter.minimum),
                                   static_cast<int>(parameter.maximum));
                }
                spin->setSuffix(parameter.suffix);
                spin->setValue(storedValue.toInt());
                editor = spin;
            }
            else if (parameter.type == scopeone::core::SignalParameterType::Real)
            {
                auto* spin = new QDoubleSpinBox(this);
                if (parameter.hasRange)
                {
                    spin->setRange(parameter.minimum, parameter.maximum);
                }
                spin->setSuffix(parameter.suffix);
                spin->setValue(storedValue.toDouble());
                editor = spin;
            }
            else if (parameter.type == scopeone::core::SignalParameterType::Choice)
            {
                auto* combo = new QComboBox(this);
                for (int index = 0; index < parameter.choices.size(); ++index)
                {
                    const QString name = index < parameter.choiceNames.size()
                                             ? parameter.choiceNames[index]
                                             : parameter.choices[index].toString();
                    combo->addItem(name, parameter.choices[index]);
                }
                combo->setCurrentIndex(std::max(0, combo->findData(storedValue)));
                editor = combo;
            }
            else
            {
                auto* edit = new QLineEdit(storedValue.toString(), this);
                editor = edit;
                if (parameter.type == scopeone::core::SignalParameterType::File)
                {
                    auto* row = new QWidget(this);
                    auto* rowLayout = new QHBoxLayout(row);
                    rowLayout->setContentsMargins(0, 0, 0, 0);
                    rowLayout->setSpacing(4);
                    auto* browse = new QToolButton(row);
                    browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
                    browse->setToolTip(tr("Select file"));
                    rowLayout->addWidget(edit, 1);
                    rowLayout->addWidget(browse);
                    connect(browse, &QToolButton::clicked, this,
                            [this, edit, parameter]()
                            {
                                const QString path = QFileDialog::getOpenFileName(
                                    this,
                                    parameter.name,
                                    QFileInfo(edit->text()).absolutePath(),
                                    parameter.fileFilter);
                                if (!path.isEmpty())
                                {
                                    edit->setText(QDir::toNativeSeparators(path));
                                }
                            });
                    m_sourceForm->addRow(parameter.name, row);
                    m_parameterEditors.insert(parameter.key, edit);
                    continue;
                }
            }
            m_sourceForm->addRow(parameter.name, editor);
            m_parameterEditors.insert(parameter.key, editor);
        }
        m_sampleIntervalSpin->setEnabled(
            descriptor.streamType == scopeone::core::SignalStreamType::TimestampedEvents);
    }

    void SignalMonitorWidget::startAcquisition()
    {
        const auto descriptor = currentDescriptor();
        if (descriptor.id.isEmpty())
        {
            return;
        }

        scopeone::core::SignalAcquisitionConfig config;
        config.sourceId = descriptor.id;
        config.sampleIntervalSeconds = m_sampleIntervalSpin->value() / 1000.0;
        config.scanImage.enabled = m_scanImageCheck->isChecked();
        config.scanImage.width = m_scanWidthSpin->value();
        config.scanImage.height = m_scanHeightSpin->value();
        config.scanImage.frameStartMarker =
            m_frameStartMarkerCombo->currentData().toUInt();
        config.scanImage.lineStartMarker =
            m_lineStartMarkerCombo->currentData().toUInt();
        config.scanImage.lineEndMarker =
            m_lineEndMarkerCombo->currentData().toUInt();
        config.scanImage.frameEndMarker =
            m_frameEndMarkerCombo->currentData().toUInt();
        config.scanImage.serpentine = m_serpentineCheck->isChecked();
        config.sourceSettings = sourceSettings();

        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (auto it = config.sourceSettings.constBegin();
             it != config.sourceSettings.constEnd(); ++it)
        {
            settings.setValue(settingKey(descriptor.id, it.key()), it.value());
        }

        m_plot->clear();
        m_valueLabel->setText(QStringLiteral("%1: -").arg(descriptor.quantity));
        QString errorMessage;
        if (!m_core->startSignalTrace(config, &errorMessage))
        {
            m_statusLabel->setText(errorMessage);
            qWarning().noquote() << errorMessage;
            return;
        }
        m_activeSourceId = descriptor.id;
    }

    void SignalMonitorWidget::handleTimeSeries(
        const scopeone::core::TimeSeriesChunk& chunk)
    {
        if (!chunk.isValid() || chunk.sourceId != m_activeSourceId)
        {
            return;
        }
        const double latestValue = m_plot->appendChunk(
            chunk, m_windowDurationSpin->value());
        m_valueLabel->setText(QStringLiteral("%1: %2")
                              .arg(chunk.quantity, formattedValue(latestValue, chunk.unit)));
        m_valueLabel->setToolTip(tr("Input events: %1\nMarkers: %2")
                                 .arg(chunk.totalInputEvents)
                                 .arg(chunk.totalMarkers));
    }

    void SignalMonitorWidget::handleStateChanged(
        const QString& sourceId,
        scopeone::core::SignalSourceState state,
        const QString& message)
    {
        if (sourceId != m_activeSourceId
            && sourceId != m_sourceCombo->currentData().toString())
        {
            return;
        }
        m_statusLabel->setText(message);
        const bool active = state == scopeone::core::SignalSourceState::Starting
            || state == scopeone::core::SignalSourceState::Running
            || state == scopeone::core::SignalSourceState::Stopping;
        setSourceControlsEnabled(!active);
        m_startButton->setEnabled(!active);
        m_stopButton->setEnabled(state == scopeone::core::SignalSourceState::Starting
                                 || state == scopeone::core::SignalSourceState::Running);
        if (active)
        {
            m_activeSourceId = sourceId;
        }
        else if (m_activeSourceId == sourceId)
        {
            m_activeSourceId.clear();
        }
    }

    void SignalMonitorWidget::setSourceControlsEnabled(bool enabled)
    {
        m_sourceCombo->setEnabled(enabled);
        for (QWidget* editor : m_parameterEditors)
        {
            editor->setEnabled(enabled);
            if (QWidget* row = editor->parentWidget(); row && row != this)
            {
                row->setEnabled(enabled);
            }
        }
        m_sampleIntervalSpin->setEnabled(
            enabled && currentDescriptor().streamType
            == scopeone::core::SignalStreamType::TimestampedEvents);
        m_scanImageCheck->setEnabled(enabled);
        const bool scanEditable = enabled && m_scanImageCheck->isChecked();
        m_scanWidthSpin->setEnabled(scanEditable);
        m_scanHeightSpin->setEnabled(scanEditable);
        m_frameStartMarkerCombo->setEnabled(scanEditable);
        m_lineStartMarkerCombo->setEnabled(scanEditable);
        m_lineEndMarkerCombo->setEnabled(scanEditable);
        m_frameEndMarkerCombo->setEnabled(scanEditable);
        m_serpentineCheck->setEnabled(scanEditable);
    }

    scopeone::core::SignalSourceDescriptor SignalMonitorWidget::currentDescriptor() const
    {
        const QString sourceId = m_sourceCombo->currentData().toString();
        for (const auto& descriptor : m_core->signalSources())
        {
            if (descriptor.id == sourceId)
            {
                return descriptor;
            }
        }
        return {};
    }

    QVariantMap SignalMonitorWidget::sourceSettings() const
    {
        QVariantMap values;
        for (auto it = m_parameterEditors.constBegin();
             it != m_parameterEditors.constEnd(); ++it)
        {
            if (const auto* spin = qobject_cast<QSpinBox*>(it.value()))
            {
                values.insert(it.key(), spin->value());
            }
            else if (const auto* spin = qobject_cast<QDoubleSpinBox*>(it.value()))
            {
                values.insert(it.key(), spin->value());
            }
            else if (const auto* combo = qobject_cast<QComboBox*>(it.value()))
            {
                values.insert(it.key(), combo->currentData());
            }
            else if (const auto* edit = qobject_cast<QLineEdit*>(it.value()))
            {
                values.insert(it.key(), edit->text().trimmed());
            }
        }
        return values;
    }
}
