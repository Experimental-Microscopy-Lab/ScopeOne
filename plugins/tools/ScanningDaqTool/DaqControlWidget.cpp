#include "DaqControlWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDebug>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace scopeone::plugins
{
    namespace
    {
        QComboBox* editableCombo(const QStringList& values, QWidget* parent)
        {
            auto* combo = new QComboBox(parent);
            combo->setEditable(true);
            combo->addItem(QString());
            combo->addItems(values);
            return combo;
        }

        QDoubleSpinBox* realSpin(double minimum,
                                 double maximum,
                                 double value,
                                 int decimals,
                                 const QString& suffix,
                                 QWidget* parent)
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setRange(minimum, maximum);
            spin->setDecimals(decimals);
            spin->setValue(value);
            spin->setSuffix(suffix);
            return spin;
        }

        QStringList commaSeparated(const QString& text)
        {
            QStringList result;
            for (const QString& item : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                const QString trimmed = item.trimmed();
                if (!trimmed.isEmpty())
                {
                    result.append(trimmed);
                }
            }
            return result;
        }
    }

    DaqControlWidget::DaqControlWidget(scopeone::core::ScopeOneCore* core,
                                       QWidget* parent)
        : QWidget(parent)
          , m_core(core)
    {
        auto* layout = new QVBoxLayout(this);
        auto* deviceForm = new QFormLayout();
        m_deviceCombo = new QComboBox(this);
        deviceForm->addRow(tr("Device"), m_deviceCombo);
        m_productLabel = new QLabel(this);
        m_productLabel->setWordWrap(true);
        deviceForm->addRow(tr("Hardware"), m_productLabel);
        m_resourcesLabel = new QLabel(this);
        m_resourcesLabel->setWordWrap(true);
        deviceForm->addRow(tr("Resources"), m_resourcesLabel);
        layout->addLayout(deviceForm);

        m_rasterGroup = new QGroupBox(tr("Raster scan timing"), this);
        m_rasterGroup->setCheckable(true);
        m_rasterGroup->setChecked(false);
        auto* rasterForm = new QFormLayout(m_rasterGroup);
        m_lineClockCombo = editableCombo({}, m_rasterGroup);
        rasterForm->addRow(tr("Line clock input"), m_lineClockCombo);
        m_lineRateSpin = realSpin(0.001, 100000000.0, 1000.0, 3,
                                  tr(" Hz"), m_rasterGroup);
        rasterForm->addRow(tr("Nominal line rate"), m_lineRateSpin);
        m_activeLinesSpin = new QSpinBox(m_rasterGroup);
        m_activeLinesSpin->setRange(2, 1000000);
        m_activeLinesSpin->setValue(512);
        rasterForm->addRow(tr("Active lines"), m_activeLinesSpin);
        m_flybackLinesSpin = new QSpinBox(m_rasterGroup);
        m_flybackLinesSpin->setRange(2, 1000000);
        m_flybackLinesSpin->setValue(16);
        rasterForm->addRow(tr("Flyback lines"), m_flybackLinesSpin);
        m_yChannelCombo = new QComboBox(m_rasterGroup);
        rasterForm->addRow(tr("Y analog output"), m_yChannelCombo);
        m_yStartSpin = realSpin(-1000.0, 1000.0, -1.0, 6,
                                tr(" V"), m_rasterGroup);
        rasterForm->addRow(tr("Y start"), m_yStartSpin);
        m_yEndSpin = realSpin(-1000.0, 1000.0, 1.0, 6,
                              tr(" V"), m_rasterGroup);
        rasterForm->addRow(tr("Y end"), m_yEndSpin);
        m_frameCounterCombo = new QComboBox(m_rasterGroup);
        rasterForm->addRow(tr("Frame counter"), m_frameCounterCombo);
        m_lineOutputCombo = editableCombo({}, m_rasterGroup);
        rasterForm->addRow(tr("Line output"), m_lineOutputCombo);
        m_frameOutputCombo = editableCombo({}, m_rasterGroup);
        rasterForm->addRow(tr("Frame output"), m_frameOutputCombo);
        layout->addWidget(m_rasterGroup);

        layout->addWidget(new QLabel(tr("Counter pulse tasks"), this));
        m_pulseTable = new QTableWidget(0, 7, this);
        m_pulseTable->setHorizontalHeaderLabels(
            {tr("Counter"), tr("Output terminal"), tr("Frequency"), tr("Duty"), tr("Delay"),
             tr("Start trigger"), tr("Edge")});
        m_pulseTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_pulseTable->horizontalHeader()->setStretchLastSection(true);
        m_pulseTable->verticalHeader()->hide();
        m_pulseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(m_pulseTable);

        auto* pulseButtons = new QHBoxLayout();
        m_addPulseButton = new QPushButton(tr("Add pulse"), this);
        m_removePulseButton = new QPushButton(tr("Remove"), this);
        pulseButtons->addWidget(m_addPulseButton);
        pulseButtons->addWidget(m_removePulseButton);
        pulseButtons->addStretch();
        layout->addLayout(pulseButtons);

        layout->addWidget(new QLabel(tr("Buffered hardware tasks"), this));
        m_bufferedTable = new QTableWidget(0, 12, this);
        m_bufferedTable->setHorizontalHeaderLabels(
            {tr("Type"), tr("Channels"), tr("Minimum"), tr("Maximum"),
             tr("Sample clock"), tr("Clock edge"), tr("Rate"), tr("Samples"),
             tr("Mode"), tr("Start trigger"), tr("Trigger edge"),
             tr("Output data")});
        m_bufferedTable->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        m_bufferedTable->horizontalHeader()->setStretchLastSection(true);
        m_bufferedTable->verticalHeader()->hide();
        m_bufferedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(m_bufferedTable);

        auto* bufferedButtons = new QHBoxLayout();
        m_addBufferedButton = new QPushButton(tr("Add task"), this);
        m_removeBufferedButton = new QPushButton(tr("Remove"), this);
        bufferedButtons->addWidget(m_addBufferedButton);
        bufferedButtons->addWidget(m_removeBufferedButton);
        bufferedButtons->addStretch();
        layout->addLayout(bufferedButtons);

        layout->addWidget(new QLabel(tr("Terminal routes"), this));
        m_routeTable = new QTableWidget(0, 3, this);
        m_routeTable->setHorizontalHeaderLabels(
            {tr("Source"), tr("Destination"), tr("Polarity")});
        m_routeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_routeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_routeTable->verticalHeader()->hide();
        m_routeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(m_routeTable);

        auto* routeButtons = new QHBoxLayout();
        m_addRouteButton = new QPushButton(tr("Add route"), this);
        m_removeRouteButton = new QPushButton(tr("Remove"), this);
        routeButtons->addWidget(m_addRouteButton);
        routeButtons->addWidget(m_removeRouteButton);
        routeButtons->addStretch();
        layout->addLayout(routeButtons);

        auto* runButtons = new QHBoxLayout();
        m_startButton = new QPushButton(tr("Arm and start"), this);
        m_startButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_stopButton = new QPushButton(tr("Stop"), this);
        m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        m_stopButton->setEnabled(false);
        runButtons->addWidget(m_startButton);
        runButtons->addWidget(m_stopButton);
        layout->addLayout(runButtons);

        m_statusLabel = new QLabel(tr("No DAQ devices available"), this);
        m_statusLabel->setWordWrap(true);
        layout->addWidget(m_statusLabel);

        for (const auto& device : m_core->daqDevices())
        {
            m_deviceCombo->addItem(device.name, device.id);
            m_deviceCombo->setItemData(
                m_deviceCombo->count() - 1,
                QStringLiteral("%1, %2").arg(device.provider, device.product),
                Qt::ToolTipRole);
        }

        connect(m_deviceCombo, &QComboBox::currentIndexChanged,
                this, &DaqControlWidget::refreshDevice);
        connect(m_addPulseButton, &QPushButton::clicked,
                this, &DaqControlWidget::addPulseRow);
        connect(m_removePulseButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedRow(m_pulseTable);
        });
        connect(m_addBufferedButton, &QPushButton::clicked,
                this, &DaqControlWidget::addBufferedRow);
        connect(m_removeBufferedButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedRow(m_bufferedTable);
        });
        connect(m_addRouteButton, &QPushButton::clicked,
                this, &DaqControlWidget::addRouteRow);
        connect(m_removeRouteButton, &QPushButton::clicked, this, [this]()
        {
            removeSelectedRow(m_routeTable);
        });
        connect(m_startButton, &QPushButton::clicked,
                this, &DaqControlWidget::startSession);
        connect(m_stopButton, &QPushButton::clicked, this, [this]()
        {
            m_core->stopDaqSession(m_activeDeviceId);
        });
        connect(m_core, &scopeone::core::ScopeOneCore::daqStateChanged,
                this, &DaqControlWidget::handleStateChanged);
        connect(m_core, &scopeone::core::ScopeOneCore::daqError,
                this, [](const QString&, const QString& message)
                {
                    qCritical().noquote() << message;
                });
        connect(m_core, &scopeone::core::ScopeOneCore::daqInputDataReady,
                this, [this](const scopeone::core::DaqInputChunk& chunk)
                {
                    if (chunk.deviceId != m_activeDeviceId)
                    {
                        return;
                    }
                    if (m_inputStatusTimer.isValid()
                        && m_inputStatusTimer.elapsed() < 100)
                    {
                        return;
                    }
                    m_inputStatusTimer.restart();
                    const qsizetype values = !chunk.analogSamplesByScan.isEmpty()
                                                     ? chunk.analogSamplesByScan.size()
                                                     : chunk.digitalSamplesByScan.size();
                    m_statusLabel->setText(
                        tr("%1: received %2 values").arg(chunk.taskName).arg(values));
                });

        refreshDevice();
    }

    scopeone::core::DaqDeviceDescriptor DaqControlWidget::currentDevice() const
    {
        const QString id = m_deviceCombo->currentData().toString();
        for (const auto& device : m_core->daqDevices())
        {
            if (device.id == id)
            {
                return device;
            }
        }
        return {};
    }

    QStringList DaqControlWidget::terminalChoices(
        const scopeone::core::DaqDeviceDescriptor& device) const
    {
        QStringList choices = device.terminals;
        choices.removeDuplicates();
        std::sort(choices.begin(), choices.end(),
                  [](const QString& left, const QString& right)
                  {
                      return left.compare(right, Qt::CaseInsensitive) < 0;
                  });
        return choices;
    }

    void DaqControlWidget::refreshDevice()
    {
        m_pulseTable->setRowCount(0);
        m_bufferedTable->setRowCount(0);
        m_routeTable->setRowCount(0);
        const auto device = currentDevice();
        const bool available = !device.id.isEmpty();
        const QStringList terminals = terminalChoices(device);
        for (QComboBox* combo : {m_lineClockCombo, m_lineOutputCombo,
                                m_frameOutputCombo})
        {
            combo->clear();
            combo->addItem(QString());
            combo->addItems(terminals);
        }
        m_yChannelCombo->clear();
        m_frameCounterCombo->clear();
        for (const auto& channel : device.channels)
        {
            if (channel.type == scopeone::core::DaqChannelType::AnalogOutput)
            {
                m_yChannelCombo->addItem(channel.physicalName);
            }
            else if (channel.type == scopeone::core::DaqChannelType::CounterOutput)
            {
                m_frameCounterCombo->addItem(channel.physicalName);
            }
        }
        m_productLabel->setText(available
                                    ? QStringLiteral("%1, %2")
                                          .arg(device.provider, device.product)
                                    : tr("DAQ device not found"));
        if (available)
        {
            int counts[6]{};
            QStringList channelNames;
            for (const auto& channel : device.channels)
            {
                ++counts[static_cast<int>(channel.type)];
                channelNames.append(channel.physicalName);
            }
            m_resourcesLabel->setText(
                tr("AI %1  AO %2  DI %3  DO %4  CI %5  CO %6  Terminals %7")
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::AnalogInput)])
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::AnalogOutput)])
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::DigitalInput)])
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::DigitalOutput)])
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::CounterInput)])
                    .arg(counts[static_cast<int>(scopeone::core::DaqChannelType::CounterOutput)])
                    .arg(device.terminals.size()));
            m_resourcesLabel->setToolTip(channelNames.join(QLatin1Char('\n')));
        }
        else
        {
            m_resourcesLabel->clear();
            m_resourcesLabel->setToolTip(QString());
        }
        m_statusLabel->setText(available
                                   ? m_core->daqStateMessage(device.id)
                                   : tr("Install a DAQ device plugin and driver"));
        setControlsEnabled(available);
        m_stopButton->setEnabled(false);
    }

    void DaqControlWidget::addBufferedRow()
    {
        const auto device = currentDevice();
        const int row = m_bufferedTable->rowCount();
        m_bufferedTable->insertRow(row);

        auto* type = new QComboBox(m_bufferedTable);
        type->addItem(tr("Analog input"), QStringLiteral("AI"));
        type->addItem(tr("Analog output"), QStringLiteral("AO"));
        type->addItem(tr("Digital input"), QStringLiteral("DI"));
        type->addItem(tr("Digital output"), QStringLiteral("DO"));
        m_bufferedTable->setCellWidget(row, 0, type);

        auto* channels = new QComboBox(m_bufferedTable);
        channels->setEditable(true);
        m_bufferedTable->setCellWidget(row, 1, channels);
        m_bufferedTable->setCellWidget(
            row, 2, realSpin(-1000.0, 1000.0, -10.0, 3,
                             tr(" V"), m_bufferedTable));
        m_bufferedTable->setCellWidget(
            row, 3, realSpin(-1000.0, 1000.0, 10.0, 3,
                             tr(" V"), m_bufferedTable));
        m_bufferedTable->setCellWidget(
            row, 4, editableCombo(terminalChoices(device), m_bufferedTable));
        auto* sampleEdge = new QComboBox(m_bufferedTable);
        sampleEdge->addItem(tr("Rising"), static_cast<int>(scopeone::core::DaqEdge::Rising));
        sampleEdge->addItem(tr("Falling"), static_cast<int>(scopeone::core::DaqEdge::Falling));
        m_bufferedTable->setCellWidget(row, 5, sampleEdge);
        m_bufferedTable->setCellWidget(
            row, 6, realSpin(0.001, 1000000000.0, 1000.0, 3,
                             tr(" Hz"), m_bufferedTable));
        auto* samples = new QSpinBox(m_bufferedTable);
        samples->setRange(1, std::numeric_limits<int>::max());
        samples->setValue(1);
        m_bufferedTable->setCellWidget(row, 7, samples);
        auto* mode = new QComboBox(m_bufferedTable);
        mode->addItem(tr("Finite"), static_cast<int>(scopeone::core::DaqSampleMode::Finite));
        mode->addItem(tr("Continuous"),
                      static_cast<int>(scopeone::core::DaqSampleMode::Continuous));
        m_bufferedTable->setCellWidget(row, 8, mode);
        m_bufferedTable->setCellWidget(
            row, 9, editableCombo(terminalChoices(device), m_bufferedTable));
        auto* triggerEdge = new QComboBox(m_bufferedTable);
        triggerEdge->addItem(tr("Rising"), static_cast<int>(scopeone::core::DaqEdge::Rising));
        triggerEdge->addItem(tr("Falling"), static_cast<int>(scopeone::core::DaqEdge::Falling));
        m_bufferedTable->setCellWidget(row, 10, triggerEdge);
        auto* outputData = new QLineEdit(m_bufferedTable);
        auto* waveformAction = outputData->addAction(
            QIcon::fromTheme(QStringLiteral("office-chart-line"),
                             style()->standardIcon(QStyle::SP_FileDialogDetailedView)),
            QLineEdit::TrailingPosition);
        waveformAction->setToolTip(tr("Generate analog waveform"));
        outputData->setPlaceholderText(tr("Comma-separated samples"));
        connect(outputData, &QLineEdit::textEdited, outputData,
                [outputData]()
                {
                    outputData->setProperty("generatedAnalogSamples", QVariant());
                });
        connect(waveformAction, &QAction::triggered, this,
                [this, outputData]()
                {
                    for (int currentRow = 0;
                         currentRow < m_bufferedTable->rowCount(); ++currentRow)
                    {
                        if (m_bufferedTable->cellWidget(currentRow, 11) == outputData)
                        {
                            configureAnalogWaveform(currentRow, outputData);
                            return;
                        }
                    }
                });
        m_bufferedTable->setCellWidget(row, 11, outputData);

        const auto updateChannels = [device, type, channels, outputData,
                                     waveformAction, this]()
        {
            int row = -1;
            for (int currentRow = 0;
                 currentRow < m_bufferedTable->rowCount(); ++currentRow)
            {
                if (m_bufferedTable->cellWidget(currentRow, 11) == outputData)
                {
                    row = currentRow;
                    break;
                }
            }
            if (row < 0)
            {
                return;
            }
            const QString taskType = type->currentData().toString();
            const bool analog = taskType == QStringLiteral("AI")
                || taskType == QStringLiteral("AO");
            const bool output = taskType == QStringLiteral("AO")
                || taskType == QStringLiteral("DO");
            const auto channelType = taskType == QStringLiteral("AI")
                                         ? scopeone::core::DaqChannelType::AnalogInput
                                     : taskType == QStringLiteral("AO")
                                         ? scopeone::core::DaqChannelType::AnalogOutput
                                     : taskType == QStringLiteral("DI")
                                         ? scopeone::core::DaqChannelType::DigitalInput
                                         : scopeone::core::DaqChannelType::DigitalOutput;
            const QString current = channels->currentText();
            channels->clear();
            for (const auto& channel : device.channels)
            {
                if (channel.type == channelType)
                {
                    channels->addItem(channel.physicalName);
                }
            }
            if (!current.isEmpty())
            {
                channels->setCurrentText(current);
            }
            m_bufferedTable->cellWidget(row, 2)->setEnabled(analog);
            m_bufferedTable->cellWidget(row, 3)->setEnabled(analog);
            m_bufferedTable->cellWidget(row, 11)->setEnabled(output);
            waveformAction->setVisible(analog && output);
        };
        connect(type, &QComboBox::currentIndexChanged, this, updateChannels);
        updateChannels();
    }

    void DaqControlWidget::configureAnalogWaveform(int row, QLineEdit* outputData)
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Analog Waveform"));
        auto* layout = new QFormLayout(&dialog);

        auto* shape = new QComboBox(&dialog);
        shape->addItems({tr("Constant"), tr("Square"), tr("Sine"),
                         tr("Triangle"), tr("Ramp")});
        shape->setCurrentIndex(1);
        layout->addRow(tr("Shape"), shape);

        auto* low = realSpin(-1000.0, 1000.0, 0.0, 6, tr(" V"), &dialog);
        auto* high = realSpin(-1000.0, 1000.0, 0.1, 6, tr(" V"), &dialog);
        layout->addRow(tr("Low"), low);
        layout->addRow(tr("High"), high);

        auto* frequency = realSpin(0.001, 100000000.0, 10.0, 3,
                                   tr(" Hz"), &dialog);
        layout->addRow(tr("Frequency"), frequency);
        auto* duty = realSpin(0.001, 99.999, 50.0, 3, tr(" %"), &dialog);
        layout->addRow(tr("Duty cycle"), duty);
        auto* points = new QSpinBox(&dialog);
        points->setRange(2, 100000);
        points->setValue(100);
        layout->addRow(tr("Samples per cycle"), points);

        connect(shape, &QComboBox::currentIndexChanged, &dialog,
                [shape, low, frequency, duty]()
                {
                    const int index = shape->currentIndex();
                    low->setEnabled(index != 0);
                    frequency->setEnabled(index != 0);
                    duty->setEnabled(index == 1);
                });

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        const int count = points->value();
        const double lower = low->value();
        const double upper = high->value();
        const double range = upper - lower;
        const double dutyFraction = duty->value() / 100.0;
        QVariantList samples;
        samples.reserve(count);
        const double twoPi = 2.0 * std::acos(-1.0);
        for (int index = 0; index < count; ++index)
        {
            const double phase = static_cast<double>(index) / count;
            double value = upper;
            switch (shape->currentIndex())
            {
            case 1:
                value = phase < 1.0 - dutyFraction ? lower : upper;
                break;
            case 2:
                value = lower + range * (0.5 - 0.5 * std::cos(twoPi * phase));
                break;
            case 3:
                value = lower + range * (1.0 - std::abs(2.0 * phase - 1.0));
                break;
            case 4:
                value = lower + range * index / (count - 1);
                break;
            default:
                break;
            }
            samples.append(value);
        }
        outputData->setProperty("generatedAnalogSamples", samples);
        outputData->setText(tr("%1, %2 to %3 V, %4 Hz")
                                .arg(shape->currentText())
                                .arg(lower, 0, 'g', 6)
                                .arg(upper, 0, 'g', 6)
                                .arg(frequency->value(), 0, 'g', 6));

        qobject_cast<QDoubleSpinBox*>(m_bufferedTable->cellWidget(row, 6))
            ->setValue(frequency->value() * count);
        qobject_cast<QSpinBox*>(m_bufferedTable->cellWidget(row, 7))->setValue(count);
        auto* mode = qobject_cast<QComboBox*>(m_bufferedTable->cellWidget(row, 8));
        mode->setCurrentIndex(mode->findData(
            static_cast<int>(scopeone::core::DaqSampleMode::Continuous)));
    }

    void DaqControlWidget::addPulseRow()
    {
        const auto device = currentDevice();
        QStringList counters;
        for (const auto& channel : device.channels)
        {
            if (channel.type == scopeone::core::DaqChannelType::CounterOutput)
            {
                counters.append(channel.physicalName);
            }
        }
        const int row = m_pulseTable->rowCount();
        m_pulseTable->insertRow(row);
        auto* counter = new QComboBox(m_pulseTable);
        counter->addItems(counters);
        m_pulseTable->setCellWidget(row, 0, counter);
        m_pulseTable->setCellWidget(
            row, 1, editableCombo(terminalChoices(device), m_pulseTable));
        m_pulseTable->setCellWidget(
            row, 2, realSpin(0.001, 100000000.0, 1000.0, 3,
                             tr(" Hz"), m_pulseTable));
        m_pulseTable->setCellWidget(
            row, 3, realSpin(0.001, 99.999, 50.0, 3,
                             tr(" %"), m_pulseTable));
        m_pulseTable->setCellWidget(
            row, 4, realSpin(0.0, 1000000.0, 0.0, 3,
                             tr(" ms"), m_pulseTable));
        m_pulseTable->setCellWidget(
            row, 5, editableCombo(terminalChoices(device), m_pulseTable));
        auto* edge = new QComboBox(m_pulseTable);
        edge->addItem(tr("Rising"), static_cast<int>(scopeone::core::DaqEdge::Rising));
        edge->addItem(tr("Falling"), static_cast<int>(scopeone::core::DaqEdge::Falling));
        m_pulseTable->setCellWidget(row, 6, edge);
    }

    void DaqControlWidget::addRouteRow()
    {
        const QStringList terminals = terminalChoices(currentDevice());
        const int row = m_routeTable->rowCount();
        m_routeTable->insertRow(row);
        m_routeTable->setCellWidget(row, 0, editableCombo(terminals, m_routeTable));
        m_routeTable->setCellWidget(row, 1, editableCombo(terminals, m_routeTable));
        auto* polarity = new QComboBox(m_routeTable);
        polarity->addItem(tr("Normal"), false);
        polarity->addItem(tr("Inverted"), true);
        m_routeTable->setCellWidget(row, 2, polarity);
    }

    void DaqControlWidget::removeSelectedRow(QTableWidget* table)
    {
        const int row = table->currentRow();
        if (row >= 0)
        {
            table->removeRow(row);
        }
    }

    void DaqControlWidget::startSession()
    {
        scopeone::core::DaqSessionConfig config;
        config.deviceId = m_deviceCombo->currentData().toString();
        if (m_rasterGroup->isChecked())
        {
            scopeone::core::DaqRasterScanConfig scan;
            scan.name = QStringLiteral("ScopeOne raster scan");
            scan.lineClock = m_lineClockCombo->currentText();
            scan.nominalLineRateHz = m_lineRateSpin->value();
            scan.activeLines = static_cast<quint32>(m_activeLinesSpin->value());
            scan.flybackLines = static_cast<quint32>(m_flybackLinesSpin->value());
            scan.yChannel = m_yChannelCombo->currentText();
            scan.yStartVolts = m_yStartSpin->value();
            scan.yEndVolts = m_yEndSpin->value();
            scan.frameCounter = m_frameCounterCombo->currentText();
            scan.lineOutputTerminal = m_lineOutputCombo->currentText();
            scan.frameOutputTerminal = m_frameOutputCombo->currentText();
            config.rasterScans.append(scan);
        }
        for (int row = 0; row < m_pulseTable->rowCount(); ++row)
        {
            scopeone::core::DaqPulseTaskConfig pulse;
            pulse.name = QStringLiteral("ScopeOne Pulse %1").arg(row + 1);
            pulse.counter = qobject_cast<QComboBox*>(
                                    m_pulseTable->cellWidget(row, 0))->currentText();
            pulse.outputTerminal = qobject_cast<QComboBox*>(
                                            m_pulseTable->cellWidget(row, 1))->currentText();
            pulse.frequencyHz = qobject_cast<QDoubleSpinBox*>(
                                    m_pulseTable->cellWidget(row, 2))->value();
            pulse.dutyCycle = qobject_cast<QDoubleSpinBox*>(
                                  m_pulseTable->cellWidget(row, 3))->value() / 100.0;
            pulse.initialDelaySeconds = qobject_cast<QDoubleSpinBox*>(
                                            m_pulseTable->cellWidget(row, 4))->value()
                / 1000.0;
            pulse.startTrigger = qobject_cast<QComboBox*>(
                                     m_pulseTable->cellWidget(row, 5))->currentText();
            pulse.startEdge = static_cast<scopeone::core::DaqEdge>(
                qobject_cast<QComboBox*>(m_pulseTable->cellWidget(row, 6))
                    ->currentData().toInt());
            config.pulseTasks.append(pulse);
        }
        for (int row = 0; row < m_routeTable->rowCount(); ++row)
        {
            scopeone::core::DaqTerminalRoute route;
            route.source = qobject_cast<QComboBox*>(
                               m_routeTable->cellWidget(row, 0))->currentText();
            route.destination = qobject_cast<QComboBox*>(
                                    m_routeTable->cellWidget(row, 1))->currentText();
            route.inverted = qobject_cast<QComboBox*>(
                                 m_routeTable->cellWidget(row, 2))->currentData().toBool();
            config.routes.append(route);
        }
        for (int row = 0; row < m_bufferedTable->rowCount(); ++row)
        {
            const QString type = qobject_cast<QComboBox*>(
                                     m_bufferedTable->cellWidget(row, 0))
                                     ->currentData().toString();
            const QStringList channels = commaSeparated(
                qobject_cast<QComboBox*>(m_bufferedTable->cellWidget(row, 1))
                    ->currentText());
            scopeone::core::DaqTaskTiming timing;
            timing.sampleClock = qobject_cast<QComboBox*>(
                                     m_bufferedTable->cellWidget(row, 4))->currentText();
            timing.sampleEdge = static_cast<scopeone::core::DaqEdge>(
                qobject_cast<QComboBox*>(m_bufferedTable->cellWidget(row, 5))
                    ->currentData().toInt());
            timing.sampleRateHz = qobject_cast<QDoubleSpinBox*>(
                                      m_bufferedTable->cellWidget(row, 6))->value();
            timing.samplesPerChannel = static_cast<quint64>(
                qobject_cast<QSpinBox*>(m_bufferedTable->cellWidget(row, 7))->value());
            timing.sampleMode = static_cast<scopeone::core::DaqSampleMode>(
                qobject_cast<QComboBox*>(m_bufferedTable->cellWidget(row, 8))
                    ->currentData().toInt());
            timing.startTrigger = qobject_cast<QComboBox*>(
                                      m_bufferedTable->cellWidget(row, 9))->currentText();
            timing.startEdge = static_cast<scopeone::core::DaqEdge>(
                qobject_cast<QComboBox*>(m_bufferedTable->cellWidget(row, 10))
                    ->currentData().toInt());
            auto* outputData = qobject_cast<QLineEdit*>(
                m_bufferedTable->cellWidget(row, 11));
            const QStringList sampleText = commaSeparated(outputData->text());

            if (type == QStringLiteral("AI") || type == QStringLiteral("AO"))
            {
                scopeone::core::DaqAnalogTaskConfig task;
                task.name = QStringLiteral("ScopeOne %1 %2").arg(type).arg(row + 1);
                task.direction = type == QStringLiteral("AI")
                                     ? scopeone::core::DaqTaskDirection::Input
                                     : scopeone::core::DaqTaskDirection::Output;
                task.channels = channels;
                task.minimumVolts = qobject_cast<QDoubleSpinBox*>(
                                        m_bufferedTable->cellWidget(row, 2))->value();
                task.maximumVolts = qobject_cast<QDoubleSpinBox*>(
                                        m_bufferedTable->cellWidget(row, 3))->value();
                task.timing = timing;
                if (task.direction == scopeone::core::DaqTaskDirection::Output)
                {
                    const QVariantList generatedSamples = outputData
                                                              ->property("generatedAnalogSamples")
                                                              .toList();
                    if (!generatedSamples.isEmpty())
                    {
                        for (const QVariant& value : generatedSamples)
                        {
                            task.outputSamplesByScan.append(value.toDouble());
                        }
                    }
                    else
                    {
                        for (const QString& value : sampleText)
                        {
                            bool valid = false;
                            const double sample = value.toDouble(&valid);
                            if (!valid)
                            {
                                m_statusLabel->setText(
                                    tr("Invalid analog output sample: %1").arg(value));
                                return;
                            }
                            task.outputSamplesByScan.append(sample);
                        }
                    }
                }
                config.analogTasks.append(task);
            }
            else
            {
                scopeone::core::DaqDigitalTaskConfig task;
                task.name = QStringLiteral("ScopeOne %1 %2").arg(type).arg(row + 1);
                task.direction = type == QStringLiteral("DI")
                                     ? scopeone::core::DaqTaskDirection::Input
                                     : scopeone::core::DaqTaskDirection::Output;
                task.lines = channels;
                task.timing = timing;
                if (task.direction == scopeone::core::DaqTaskDirection::Output)
                {
                    for (const QString& value : sampleText)
                    {
                        bool valid = false;
                        const quint32 sample = value.toUInt(&valid, 0);
                        if (!valid)
                        {
                            m_statusLabel->setText(
                                tr("Invalid digital output sample: %1").arg(value));
                            return;
                        }
                        task.outputSamplesByScan.append(sample);
                    }
                }
                config.digitalTasks.append(task);
            }
        }

        QString errorMessage;
        if (!m_core->startDaqSession(config, &errorMessage))
        {
            m_statusLabel->setText(errorMessage);
            qWarning().noquote() << errorMessage;
            return;
        }
        m_activeDeviceId = config.deviceId;
    }

    void DaqControlWidget::setControlsEnabled(bool enabled)
    {
        m_deviceCombo->setEnabled(enabled);
        m_rasterGroup->setEnabled(enabled);
        m_pulseTable->setEnabled(enabled);
        m_bufferedTable->setEnabled(enabled);
        m_routeTable->setEnabled(enabled);
        m_addPulseButton->setEnabled(enabled);
        m_removePulseButton->setEnabled(enabled);
        m_addBufferedButton->setEnabled(enabled);
        m_removeBufferedButton->setEnabled(enabled);
        m_addRouteButton->setEnabled(enabled);
        m_removeRouteButton->setEnabled(enabled);
        m_startButton->setEnabled(enabled && m_deviceCombo->count() > 0);
    }

    void DaqControlWidget::handleStateChanged(const QString& deviceId,
                                              scopeone::core::DaqState state,
                                              const QString& message)
    {
        if (deviceId != m_activeDeviceId
            && deviceId != m_deviceCombo->currentData().toString())
        {
            return;
        }
        const bool active = state == scopeone::core::DaqState::Armed
            || state == scopeone::core::DaqState::Running;
        m_statusLabel->setText(message);
        setControlsEnabled(!active);
        m_stopButton->setEnabled(active);
        if (active)
        {
            m_activeDeviceId = deviceId;
        }
        else
        {
            m_activeDeviceId.clear();
        }
    }
}
