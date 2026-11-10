#include "LightSourceWidget.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QFrame>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>
#include <cmath>

namespace scopeone::ui
{
    namespace
    {
        // The unit written at the end of a property name, as in "Power Setpoint [mW]" or "Power (mW)"
        QString unitOf(const QString& propertyName)
        {
            static const QRegularExpression pattern(QStringLiteral("[\\[(]([^\\])]+)[\\])]\\s*$"));
            const QRegularExpressionMatch match = pattern.match(propertyName);
            return match.hasMatch() ? match.captured(1) : QString{};
        }
    }

    namespace
    {
        constexpr int kSliderSteps = 1000;

        // Wavelength in nm to RGB after Dan Bruton, with the intensity falling off toward 380 and
        // 780 nm. The ends outside the visible range get a dim gray-violet and a dim dark red.
        QColor wavelengthColor(double nm)
        {
            if (nm < 380.0)
            {
                return QColor(90, 80, 110);
            }
            if (nm > 780.0)
            {
                return QColor(110, 40, 40);
            }
            double r = 0.0, g = 0.0, b = 0.0;
            if (nm < 440.0) { r = (440.0 - nm) / 60.0; b = 1.0; }
            else if (nm < 490.0) { g = (nm - 440.0) / 50.0; b = 1.0; }
            else if (nm < 510.0) { g = 1.0; b = (510.0 - nm) / 20.0; }
            else if (nm < 580.0) { r = (nm - 510.0) / 70.0; g = 1.0; }
            else if (nm < 645.0) { r = 1.0; g = (645.0 - nm) / 65.0; }
            else { r = 1.0; }
            double factor = 1.0;
            if (nm < 420.0) factor = 0.3 + 0.7 * (nm - 380.0) / 40.0;
            else if (nm > 700.0) factor = 0.3 + 0.7 * (780.0 - nm) / 80.0;
            const auto channel = [factor](double c) { return int(255.0 * std::pow(c * factor, 0.8) + 0.5); };
            return QColor(channel(r), channel(g), channel(b));
        }

        // The color of a channel from the wavelength in its name, or the neutral color without one
        QColor channelColor(const QString& name, const QPalette& palette)
        {
            static const QRegularExpression number(QStringLiteral("\\d+(\\.\\d+)?"));
            const QRegularExpressionMatch match = number.match(name);
            if (match.hasMatch())
            {
                const double nm = match.captured(0).toDouble();
                if (nm >= 300.0 && nm <= 1100.0)
                {
                    return wavelengthColor(nm);
                }
            }
            return palette.color(QPalette::Mid);
        }
    }

    LightSourceWidget::LightSourceWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QGroupBox(tr("Light Sources"), parent)
        , m_core(core)
    {
        m_editButton = new QToolButton(this);
        m_editButton->setIcon(QIcon::fromTheme(QStringLiteral("document-edit")));
        m_editButton->setToolTip(tr("Edit light sources"));
        m_editButton->setAutoRaise(true);
        connect(m_editButton, &QToolButton::clicked, this, &LightSourceWidget::editRequested);
        m_layout = new QGridLayout(this);
        m_layout->setColumnStretch(3, 1);
        m_timer = new QTimer(this);
        m_timer->setInterval(1000);
        connect(m_timer, &QTimer::timeout, this, &LightSourceWidget::readMeasuredPower);
        using Core = scopeone::core::ScopeOneCore;
        connect(m_core, &Core::hardwareConfigurationChanged, this, &LightSourceWidget::rebuild);
        connect(m_core, &Core::hardwareDevicesChanged, this, &LightSourceWidget::rebuild);
        connect(m_core, &Core::deviceStateChanged, this,
                [this]()
                {
                    if (!m_core->configurationOperationRunning())
                    {
                        refreshSettings();
                    }
                },
                Qt::QueuedConnection);
        rebuild();
    }

    void LightSourceWidget::resizeEvent(QResizeEvent* event)
    {
        QGroupBox::resizeEvent(event);
        m_editButton->move(width() - m_editButton->width() - 8, 0);
    }

    // Starts the reading of the measured power when the panel becomes visible
    void LightSourceWidget::showEvent(QShowEvent* event)
    {
        QGroupBox::showEvent(event);
        updateTimer();
    }

    // Stops the reading of the measured power when the panel is hidden
    void LightSourceWidget::hideEvent(QHideEvent* event)
    {
        QGroupBox::hideEvent(event);
        updateTimer();
    }

    // Builds the rows again from the light sources of the loaded configuration
    void LightSourceWidget::rebuild()
    {
        ++m_generation;
        while (QLayoutItem* item = m_layout->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        m_rows.clear();

        for (const auto& source : m_core->lightSources())
        {
            Row row;
            row.source = source;
            const int line = m_rows.size();
            const QColor color = channelColor(source.name, palette());
            auto* bar = new QFrame(this);
            bar->setFixedWidth(4);
            bar->setAutoFillBackground(true);
            QPalette barPalette = bar->palette();
            barPalette.setColor(QPalette::Window, color);
            bar->setPalette(barPalette);
            m_layout->addWidget(bar, line, 0);
            m_layout->addWidget(new QLabel(source.name, this), line, 1);

            if (!source.enableProperty.isEmpty())
            {
                row.enableButton = new QPushButton(tr("On"), this);
                row.enableButton->setCheckable(true);
                m_layout->addWidget(row.enableButton, line, 2);
            }

            row.powerSlider = new QSlider(Qt::Horizontal, this);
            row.powerSlider->setRange(0, kSliderSteps);
            row.powerSlider->setStyleSheet(
                QStringLiteral("QSlider::sub-page:horizontal { background: %1; border-radius: 1px; }")
                    .arg(color.name()));
            m_layout->addWidget(row.powerSlider, line, 3);

            row.powerUnit = unitOf(source.powerProperty);
            row.powerSpinBox = new QDoubleSpinBox(this);
            row.powerSpinBox->setKeyboardTracking(false);
            row.powerSpinBox->setDecimals(3);
            row.powerSpinBox->setRange(0.0, 1000.0);
            if (!row.powerUnit.isEmpty())
            {
                row.powerSpinBox->setSuffix(QStringLiteral(" ") + row.powerUnit);
            }
            m_layout->addWidget(row.powerSpinBox, line, 4);

            if (!source.readingProperty.isEmpty())
            {
                row.readingUnit = unitOf(source.readingProperty);
                row.readingLabel = new QLabel(this);
                row.readingLabel->setMinimumWidth(80);
                row.readingLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                m_layout->addWidget(row.readingLabel, line, 5);
            }
            m_rows.append(row);
        }

        for (int i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            if (row.enableButton)
            {
                connect(row.enableButton, &QPushButton::clicked, this,
                        [this, i](bool checked)
                        {
                            const auto& source = m_rows[i].source;
                            write(source.enableDevice, source.enableProperty,
                                  checked ? source.onValue : source.offValue);
                        });
            }
            connect(row.powerSpinBox, &QDoubleSpinBox::editingFinished, this, [this, i]() { writePower(i); });
            connect(row.powerSpinBox, &QDoubleSpinBox::valueChanged, this,
                    [this, i](double)
                    {
                        syncSlider(m_rows[i]);
                    });
            // While dragging only the spin box follows, the device is written on release
            connect(row.powerSlider, &QSlider::valueChanged, this,
                    [this, i](int position)
                    {
                        Row& target = m_rows[i];
                        const double lower = target.powerSpinBox->minimum();
                        const double upper = target.powerSpinBox->maximum();
                        const QSignalBlocker blocker(target.powerSpinBox);
                        target.powerSpinBox->setValue(lower + (upper - lower) * position / kSliderSteps);
                        if (!target.powerSlider->isSliderDown())
                        {
                            writePower(i);
                        }
                    });
            connect(row.powerSlider, &QSlider::sliderReleased, this, [this, i]() { writePower(i); });
        }
        refreshSettings();
        setVisible(!m_rows.isEmpty());
        updateTimer();
    }

    // Reads the rows on the hardware thread and shows the result, unless the rows were rebuilt
    // meanwhile. The settings are the switch state, the power setting and the power limits.
    void LightSourceWidget::readRows(bool settings)
    {
        QList<scopeone::core::ScopeOneCore::LightSource> sources;
        for (const Row& row : m_rows)
        {
            sources.append(row.source);
        }
        const int generation = m_generation;
        auto* watcher = new QFutureWatcher<QList<scopeone::core::ScopeOneCore::LightSourceState>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, generation, settings]()
                {
                    if (!settings)
                    {
                        m_reading = false;
                    }
                    watcher->deleteLater();
                    if (generation == m_generation)
                    {
                        applyStates(watcher->result(), settings);
                    }
                });
        watcher->setFuture(m_core->readLightSources(sources, settings));
    }

    void LightSourceWidget::refreshSettings()
    {
        if (!m_rows.isEmpty())
        {
            readRows(true);
        }
    }

    // Shows the values of a round on the rows, the settings only when the round read them
    void LightSourceWidget::applyStates(const QList<scopeone::core::ScopeOneCore::LightSourceState>& states,
                                        bool settings)
    {
        for (int i = 0; i < m_rows.size() && i < states.size(); ++i)
        {
            Row& row = m_rows[i];
            const auto& state = states[i];
            if (row.readingLabel)
            {
                row.readingLabel->setText(formatReading(state.reading, row.readingUnit));
            }
            if (!settings)
            {
                continue;
            }
            if (row.enableButton)
            {
                row.enableButton->setChecked(state.enable == row.source.onValue);
            }
            // The limits are applied only when the property has them, else the range stays as built
            if (state.hasLimits)
            {
                row.powerSpinBox->setRange(state.lower, state.upper);
            }
            row.lastPower = state.power.toDouble();
            if (!row.powerSpinBox->hasFocus() && !row.powerSlider->isSliderDown())
            {
                row.powerSpinBox->setValue(row.lastPower);
                // A new range with the same value emits no valueChanged, which left the slider
                // at the position of the old range
                syncSlider(row);
            }
        }
    }

    // Writes the power of a row when it differs from the last value read or written
    void LightSourceWidget::writePower(int index)
    {
        Row& row = m_rows[index];
        // The value is compared as a number with the last one read or written. Comparing
        // the device string "50.000" with "50" wrote to the device on every focus change.
        const double value = row.powerSpinBox->value();
        if (value != row.lastPower)
        {
            row.lastPower = value;
            write(row.source.powerDevice, row.source.powerProperty, QString::number(value, 'g', 10));
        }
    }

    // Puts the slider at the position of the spin box value
    void LightSourceWidget::syncSlider(Row& row)
    {
        const double lower = row.powerSpinBox->minimum();
        const double upper = row.powerSpinBox->maximum();
        const QSignalBlocker blocker(row.powerSlider);
        row.powerSlider->setValue(upper > lower
                                      ? int(std::lround((row.powerSpinBox->value() - lower) / (upper - lower) * kSliderSteps))
                                      : 0);
    }

    // Runs the timer of the measured power only while the panel is visible and has a reading
    void LightSourceWidget::updateTimer()
    {
        bool reading = false;
        for (const Row& row : m_rows)
        {
            reading = reading || row.readingLabel;
        }
        if (reading && isVisible())
        {
            if (!m_timer->isActive())
            {
                m_timer->start();
                readMeasuredPower();
            }
        }
        else
        {
            m_timer->stop();
        }
    }

    // Reads the measured power of all rows. A round is skipped while the previous one is still
    // reading.
    void LightSourceWidget::readMeasuredPower()
    {
        if (m_reading || m_rows.isEmpty() || m_core->configurationOperationRunning())
        {
            return;
        }
        m_reading = true;
        readRows(false);
    }

    // Writes one property in the thread pool and reports a failure
    void LightSourceWidget::write(const QString& device, const QString& property, const QString& value)
    {
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                [this, watcher, device, property]()
                {
                    const QString error = watcher->result();
                    watcher->deleteLater();
                    if (!error.isNull())
                    {
                        emit writeFailed(tr("Failed to set %1 of %2: %3").arg(property, device, error));
                    }
                    refreshSettings();
                });
        watcher->setFuture(m_core->writeProperty(device, property, value));
    }

    // Shows a measured value with the unit of its property
    QString LightSourceWidget::formatReading(const QString& value, const QString& unit) const
    {
        bool number = false;
        const double parsed = value.toDouble(&number);
        const QString text = number ? QLocale().toString(parsed, 'g', 4) : value;
        return unit.isEmpty() ? text : text + QStringLiteral(" ") + unit;
    }
}
