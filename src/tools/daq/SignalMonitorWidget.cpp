#include "SignalMonitorWidget.h"

#include "tools/ToolContext.h"
#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace scopeone::ui
{
    namespace
    {
        QString parameterSettingKey(const QString& sourceId, const QString& parameterKey)
        {
            return QStringLiteral("signalSources/%1/%2").arg(sourceId, parameterKey);
        }

        QString monitorKey(const QString& key)
        {
            return QStringLiteral("signalMonitor/%1").arg(key);
        }

        QString channelKey(int index, const QString& key)
        {
            return QStringLiteral("signalMonitor/channel%1/%2").arg(index + 1).arg(key);
        }

        // The 1, 2, 5 steps of a scope knob from the first to the last value
        QVector<double> knobSteps(int firstExponent, int lastExponent)
        {
            QVector<double> steps;
            for (int exponent = firstExponent; exponent <= lastExponent; ++exponent)
            {
                for (const double mantissa : {1.0, 2.0, 5.0})
                {
                    steps.append(mantissa * std::pow(10.0, exponent));
                }
            }
            return steps;
        }

        void selectNearest(QComboBox* combo, double value)
        {
            int best = 0;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (int index = 0; index < combo->count(); ++index)
            {
                const double distance = std::abs(std::log(combo->itemData(index).toDouble() / value));
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = index;
                }
            }
            combo->setCurrentIndex(best);
        }

        QComboBox* channelCombo(QWidget* parent)
        {
            auto* combo = new QComboBox(parent);
            for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
            {
                combo->addItem(QStringLiteral("CH%1").arg(index + 1), index);
            }
            return combo;
        }

        QDoubleSpinBox* valueSpin(QWidget* parent)
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setRange(-1e6, 1e6);
            spin->setDecimals(4);
            spin->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
            return spin;
        }
    }

    SignalMonitorWidget::SignalMonitorWidget(scopeone::core::ScopeOneCore* core,
                                             scopeone::ui::ScopeOneToolContext& context,
                                             QWidget* parent)
        : QWidget(parent)
          , m_core(core)
          , m_context(context)
    {
        auto* layout = new QHBoxLayout(this);
        m_view = new OscilloscopeView(this);
        layout->addWidget(m_view, 1);

        auto* panel = new QWidget();
        auto* panelLayout = new QVBoxLayout(panel);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumWidth(320);
        scroll->setWidget(panel);
        layout->addWidget(scroll);

        auto* acquisitionBox = new QGroupBox(tr("Acquisition"), panel);
        auto* acquisitionForm = new QFormLayout(acquisitionBox);
        m_sampleIntervalSpin = new QDoubleSpinBox(acquisitionBox);
        m_sampleIntervalSpin->setRange(0.001, 10000.0);
        m_sampleIntervalSpin->setDecimals(3);
        m_sampleIntervalSpin->setSuffix(tr(" ms"));
        m_sampleIntervalSpin->setValue(1.0);
        m_sampleIntervalSpin->setToolTip(
            tr("The sample clock of a time series such as a DAQ input, and the bin of an event source"));
        acquisitionForm->addRow(tr("Interval"), m_sampleIntervalSpin);
        auto* acquisitionButtons = new QHBoxLayout();
        m_startButton = new QPushButton(tr("Start"), acquisitionBox);
        m_startButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_stopButton = new QPushButton(tr("Stop"), acquisitionBox);
        m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        acquisitionButtons->addWidget(m_startButton);
        acquisitionButtons->addWidget(m_stopButton);
        acquisitionForm->addRow(acquisitionButtons);
        m_statusLabel = new QLabel(tr("No signal sources available"), acquisitionBox);
        m_statusLabel->setWordWrap(true);
        acquisitionForm->addRow(m_statusLabel);
        panelLayout->addWidget(acquisitionBox);

        auto* horizontalBox = new QGroupBox(tr("Horizontal"), panel);
        auto* horizontalForm = new QFormLayout(horizontalBox);
        m_timebaseCombo = new QComboBox(horizontalBox);
        for (const double step : knobSteps(-6, 1))
        {
            m_timebaseCombo->addItem(
                QStringLiteral("%1/div").arg(OscilloscopeView::formatValue(step, QStringLiteral("s"))), step);
        }
        horizontalForm->addRow(tr("Timebase"), m_timebaseCombo);
        m_triggerPositionSpin = new QDoubleSpinBox(horizontalBox);
        m_triggerPositionSpin->setRange(0.0, 100.0);
        m_triggerPositionSpin->setDecimals(0);
        m_triggerPositionSpin->setSuffix(tr(" %"));
        m_triggerPositionSpin->setValue(50.0);
        m_triggerPositionSpin->setToolTip(tr("Where the trigger sits on the screen, from the left edge"));
        horizontalForm->addRow(tr("Trigger position"), m_triggerPositionSpin);
        panelLayout->addWidget(horizontalBox);

        auto* triggerBox = new QGroupBox(tr("Trigger"), panel);
        auto* triggerForm = new QFormLayout(triggerBox);
        m_triggerModeCombo = new QComboBox(triggerBox);
        m_triggerModeCombo->addItem(tr("Roll"), static_cast<int>(ScopeTriggerMode::Roll));
        m_triggerModeCombo->addItem(tr("Auto"), static_cast<int>(ScopeTriggerMode::Auto));
        m_triggerModeCombo->addItem(tr("Normal"), static_cast<int>(ScopeTriggerMode::Normal));
        m_triggerModeCombo->setToolTip(
            tr("Roll shows the newest data scrolling in; Auto and Normal hold the screen on an edge, "
               "and Auto also shows the signal when no edge comes"));
        triggerForm->addRow(tr("Mode"), m_triggerModeCombo);
        m_triggerChannelCombo = channelCombo(triggerBox);
        triggerForm->addRow(tr("Source"), m_triggerChannelCombo);
        m_triggerSlopeCombo = new QComboBox(triggerBox);
        m_triggerSlopeCombo->addItem(tr("Rising"), static_cast<int>(ScopeTriggerSlope::Rising));
        m_triggerSlopeCombo->addItem(tr("Falling"), static_cast<int>(ScopeTriggerSlope::Falling));
        triggerForm->addRow(tr("Slope"), m_triggerSlopeCombo);
        m_triggerLevelSpin = valueSpin(triggerBox);
        triggerForm->addRow(tr("Level"), m_triggerLevelSpin);
        auto* displayButtons = new QHBoxLayout();
        m_runButton = new QPushButton(tr("Stop"), triggerBox);
        m_runButton->setToolTip(tr("Hold or resume the screen; the sources keep acquiring"));
        m_singleButton = new QPushButton(tr("Single"), triggerBox);
        m_singleButton->setToolTip(tr("Show the next edge and hold the screen"));
        displayButtons->addWidget(m_runButton);
        displayButtons->addWidget(m_singleButton);
        triggerForm->addRow(displayButtons);
        panelLayout->addWidget(triggerBox);

        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            panelLayout->addWidget(createChannelBox(index));
        }

        auto* displayBox = new QGroupBox(tr("Display"), panel);
        auto* displayForm = new QFormLayout(displayBox);
        m_autoScaleButton = new QPushButton(tr("Auto Scale"), displayBox);
        m_autoScaleButton->setToolTip(tr("Fit the scale and offset of each channel to the screen"));
        displayForm->addRow(m_autoScaleButton);
        m_cursorsCheck = new QCheckBox(tr("Show cursors"), displayBox);
        m_cursorsCheck->setToolTip(tr("Drag the cursors on the screen"));
        displayForm->addRow(tr("Cursors"), m_cursorsCheck);
        m_cursorChannelCombo = channelCombo(displayBox);
        displayForm->addRow(tr("Cursor channel"), m_cursorChannelCombo);
        panelLayout->addWidget(displayBox);

        m_scanGroup = new QGroupBox(tr("Scan image from CH1"), panel);
        m_scanGroup->setCheckable(true);
        m_scanGroup->setChecked(false);
        auto* scanForm = new QFormLayout(m_scanGroup);
        m_scanWidthSpin = new QSpinBox(m_scanGroup);
        m_scanWidthSpin->setRange(1, 8192);
        m_scanWidthSpin->setValue(256);
        scanForm->addRow(tr("Width"), m_scanWidthSpin);
        m_scanHeightSpin = new QSpinBox(m_scanGroup);
        m_scanHeightSpin->setRange(1, 8192);
        m_scanHeightSpin->setValue(256);
        scanForm->addRow(tr("Height"), m_scanHeightSpin);
        m_scanGainSpin = new QSpinBox(m_scanGroup);
        m_scanGainSpin->setRange(1, 65535);
        m_scanGainSpin->setValue(1000);
        m_scanGainSpin->setSuffix(tr(" x"));
        scanForm->addRow(tr("Gain"), m_scanGainSpin);
        m_scanAverageFramesSpin = new QSpinBox(m_scanGroup);
        m_scanAverageFramesSpin->setRange(1, 256);
        m_scanAverageFramesSpin->setValue(1);
        m_scanAverageFramesSpin->setToolTip(
            tr("Average this many complete scan images before publishing one result"));
        scanForm->addRow(tr("Average frames"), m_scanAverageFramesSpin);
        const auto markerCombo = [this](quint32 value)
        {
            auto* combo = new QComboBox(m_scanGroup);
            combo->addItem(tr("Off"), 0U);
            combo->addItem(tr("Marker 1"), 1U);
            combo->addItem(tr("Marker 2"), 2U);
            combo->addItem(tr("Marker 3"), 4U);
            combo->addItem(tr("Marker 4"), 8U);
            combo->setCurrentIndex(combo->findData(value));
            return combo;
        };
        m_frameStartMarkerCombo = markerCombo(1);
        scanForm->addRow(tr("Frame start"), m_frameStartMarkerCombo);
        m_lineMarkerCombo = markerCombo(2);
        m_lineMarkerCombo->setToolTip(tr("One marker completes one full back-and-forth scan line"));
        scanForm->addRow(tr("Line marker"), m_lineMarkerCombo);
        m_frameEndMarkerCombo = markerCombo(0);
        scanForm->addRow(tr("Frame end"), m_frameEndMarkerCombo);
        m_serpentineCheck = new QCheckBox(tr("Reverse alternate lines"), m_scanGroup);
        scanForm->addRow(tr("Scan order"), m_serpentineCheck);
        m_mirrorHorizontalCheck = new QCheckBox(tr("Mirror left/right"), m_scanGroup);
        scanForm->addRow(tr("Image orientation"), m_mirrorHorizontalCheck);
        panelLayout->addWidget(m_scanGroup);
        panelLayout->addStretch(1);

        loadSettings();

        connect(m_startButton, &QPushButton::clicked, this, &SignalMonitorWidget::startAcquisition);
        connect(m_stopButton, &QPushButton::clicked, this, &SignalMonitorWidget::stopAcquisition);
        connect(m_autoScaleButton, &QPushButton::clicked, this, &SignalMonitorWidget::autoScale);
        connect(m_runButton, &QPushButton::clicked, this, [this]()
        {
            m_view->setRunning(!m_view->isRunning());
        });
        connect(m_singleButton, &QPushButton::clicked, m_view, &OscilloscopeView::armSingle);
        connect(m_view, &OscilloscopeView::runningChanged, this, [this](bool running)
        {
            m_runButton->setText(running ? tr("Stop") : tr("Run"));
        });
        connect(m_timebaseCombo, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);
        connect(m_triggerPositionSpin, &QDoubleSpinBox::valueChanged, this, &SignalMonitorWidget::applyView);
        connect(m_triggerModeCombo, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);
        connect(m_triggerChannelCombo, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::updateUnits);
        connect(m_triggerSlopeCombo, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);
        connect(m_triggerLevelSpin, &QDoubleSpinBox::valueChanged, this, &SignalMonitorWidget::applyView);
        connect(m_cursorsCheck, &QCheckBox::toggled, this, &SignalMonitorWidget::applyView);
        connect(m_cursorChannelCombo, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);

        connect(m_core, &scopeone::core::ScopeOneCore::signalSourcesChanged,
                this, &SignalMonitorWidget::refreshSources);
        connect(m_core, &scopeone::core::ScopeOneCore::signalTimeSeriesReady,
                this, &SignalMonitorWidget::handleTimeSeries);
        connect(m_core, &scopeone::core::ScopeOneCore::signalSourceStateChanged,
                this, &SignalMonitorWidget::handleStateChanged);
        connect(m_core, &scopeone::core::ScopeOneCore::signalSourceError,
                this, [](const QString&, const QString& message)
                {
                    qCritical().noquote() << message;
                });
        connect(m_core, &scopeone::core::ScopeOneCore::staticFramePublished,
                this,
                [this](const QString& sourceId, const QString&, const scopeone::core::ImageFrame&)
                {
                    if (m_scanSourceId.isEmpty()
                        || m_scanLayerShown
                        || sourceId != QStringLiteral("scan:%1").arg(m_scanSourceId))
                    {
                        return;
                    }
                    m_scanLayerShown = true;
                    m_context.showLayers({scopeone::core::ScopeOneCore::staticLayerKey(sourceId)});
                });
        connect(m_core, &scopeone::core::ScopeOneCore::scanImageSessionReady,
                this, [this](const std::shared_ptr<scopeone::core::RecordingSessionData>& session)
                {
                    m_statusLabel->setText(tr("Scan session ready: %1 frames")
                                               .arg(session->recordedFrameCount()));
                    m_context.presentSession(session, tr("Reconstructed Scan"));
                });

        refreshSources();
    }

    SignalMonitorWidget::~SignalMonitorWidget()
    {
        saveSettings();
    }

    QWidget* SignalMonitorWidget::createChannelBox(int index)
    {
        ChannelControls& channel = m_channels[index];
        channel.box = new QGroupBox(QStringLiteral("CH%1").arg(index + 1), this);
        channel.box->setCheckable(true);
        channel.box->setChecked(index == 0);
        channel.box->setStyleSheet(QStringLiteral("QGroupBox::title { color: %1; }")
                                       .arg(OscilloscopeView::channelColor(index).name()));
        auto* form = new QFormLayout(channel.box);
        channel.source = new QComboBox(channel.box);
        form->addRow(tr("Source"), channel.source);
        channel.scale = new QComboBox(channel.box);
        for (const double step : knobSteps(-6, 5))
        {
            channel.scale->addItem(QString(), step);
        }
        selectNearest(channel.scale, 1.0);
        form->addRow(tr("Scale"), channel.scale);
        channel.offset = valueSpin(channel.box);
        channel.offset->setToolTip(tr("Added to the signal before it is drawn"));
        form->addRow(tr("Offset"), channel.offset);
        channel.coupling = new QComboBox(channel.box);
        channel.coupling->addItem(tr("DC"), false);
        channel.coupling->addItem(tr("AC"), true);
        channel.coupling->setToolTip(tr("AC draws the signal without its mean over the screen"));
        form->addRow(tr("Coupling"), channel.coupling);
        channel.parameterForm = new QFormLayout();
        form->addRow(channel.parameterForm);

        connect(channel.box, &QGroupBox::toggled, this, [this]()
        {
            applyView();
            updateControls();
        });
        connect(channel.source, &QComboBox::currentIndexChanged, this, [this, index]()
        {
            m_channels[index].preferredSource = channelSource(index);
            rebuildParameters(index);
            updateUnits();
        });
        connect(channel.scale, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);
        connect(channel.offset, &QDoubleSpinBox::valueChanged, this, &SignalMonitorWidget::applyView);
        connect(channel.coupling, &QComboBox::currentIndexChanged, this, &SignalMonitorWidget::applyView);
        return channel.box;
    }

    void SignalMonitorWidget::refreshSources()
    {
        m_sources = m_core->signalSources();
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            ChannelControls& channel = m_channels[index];
            {
                const QSignalBlocker blocker(channel.source);
                channel.source->clear();
                for (const auto& source : m_sources)
                {
                    channel.source->addItem(source.name, source.id);
                    channel.source->setItemData(channel.source->count() - 1,
                                                QStringLiteral("%1, %2 [%3]")
                                                    .arg(source.provider, source.quantity, source.unit),
                                                Qt::ToolTipRole);
                }
                int selected = channel.source->findData(channel.preferredSource);
                if (selected < 0 && !m_sources.isEmpty())
                {
                    // A new channel shows the next source, so the channels start on different ones
                    selected = std::min<int>(index, m_sources.size() - 1);
                }
                channel.source->setCurrentIndex(selected);
            }
            rebuildParameters(index);
        }
        updateUnits();
        updateControls();
        updateStatus();
    }

    void SignalMonitorWidget::rebuildParameters(int index)
    {
        ChannelControls& channel = m_channels[index];
        while (channel.parameterForm->rowCount() > 0)
        {
            channel.parameterForm->removeRow(0);
        }
        channel.editors.clear();

        const auto source = descriptor(channelSource(index));
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (const auto& parameter : source.parameters)
        {
            const QVariant storedValue = settings.value(
                parameterSettingKey(source.id, parameter.key), parameter.defaultValue);
            QWidget* editor = nullptr;
            QWidget* row = nullptr;
            if (parameter.type == scopeone::core::SignalParameterType::Integer)
            {
                auto* spin = new QSpinBox(channel.box);
                if (parameter.hasRange)
                {
                    spin->setRange(static_cast<int>(parameter.minimum), static_cast<int>(parameter.maximum));
                }
                spin->setSuffix(parameter.suffix);
                spin->setValue(storedValue.toInt());
                editor = spin;
            }
            else if (parameter.type == scopeone::core::SignalParameterType::Real)
            {
                auto* spin = new QDoubleSpinBox(channel.box);
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
                auto* combo = new QComboBox(channel.box);
                for (int choice = 0; choice < parameter.choices.size(); ++choice)
                {
                    const QString name = choice < parameter.choiceNames.size()
                                             ? parameter.choiceNames[choice]
                                             : parameter.choices[choice].toString();
                    combo->addItem(name, parameter.choices[choice]);
                }
                combo->setCurrentIndex(std::max(0, combo->findData(storedValue)));
                editor = combo;
            }
            else
            {
                auto* edit = new QLineEdit(storedValue.toString(), channel.box);
                editor = edit;
                if (parameter.type == scopeone::core::SignalParameterType::File)
                {
                    row = new QWidget(channel.box);
                    auto* rowLayout = new QHBoxLayout(row);
                    rowLayout->setContentsMargins(0, 0, 0, 0);
                    rowLayout->setSpacing(4);
                    auto* browse = new QToolButton(row);
                    browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
                    browse->setToolTip(tr("Select file"));
                    rowLayout->addWidget(edit, 1);
                    rowLayout->addWidget(browse);
                    connect(browse, &QToolButton::clicked, this, [this, edit, parameter]()
                    {
                        const QString path = QFileDialog::getOpenFileName(
                            this, parameter.name, QFileInfo(edit->text()).absolutePath(), parameter.fileFilter);
                        if (!path.isEmpty())
                        {
                            edit->setText(QDir::toNativeSeparators(path));
                        }
                    });
                }
            }
            channel.parameterForm->addRow(parameter.name, row ? row : editor);
            channel.editors.insert(parameter.key, editor);
        }
        updateControls();
    }

    // The scale, offset and trigger level are in the unit of the source their channel shows
    void SignalMonitorWidget::updateUnits()
    {
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            ChannelControls& channel = m_channels[index];
            const QString unit = descriptor(channelSource(index)).unit;
            const QSignalBlocker blocker(channel.scale);
            for (int step = 0; step < channel.scale->count(); ++step)
            {
                channel.scale->setItemText(
                    step, QStringLiteral("%1/div").arg(
                              OscilloscopeView::formatValue(channel.scale->itemData(step).toDouble(), unit)));
            }
            channel.offset->setSuffix(QStringLiteral(" %1").arg(unit));
        }
        m_triggerLevelSpin->setSuffix(QStringLiteral(" %1").arg(
            descriptor(channelSource(m_triggerChannelCombo->currentIndex())).unit));
        applyView();
    }

    void SignalMonitorWidget::applyView()
    {
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            const ChannelControls& controls = m_channels[index];
            ScopeChannel channel;
            channel.enabled = controls.box->isChecked();
            channel.sourceId = channelSource(index);
            channel.unit = descriptor(channel.sourceId).unit;
            channel.scale = controls.scale->currentData().toDouble();
            channel.offset = controls.offset->value();
            channel.acCoupled = controls.coupling->currentData().toBool();
            m_view->setChannel(index, channel);
        }
        m_view->setTimebase(m_timebaseCombo->currentData().toDouble());
        m_view->setTrigger(static_cast<ScopeTriggerMode>(m_triggerModeCombo->currentData().toInt()),
                           m_triggerChannelCombo->currentIndex(),
                           static_cast<ScopeTriggerSlope>(m_triggerSlopeCombo->currentData().toInt()),
                           m_triggerLevelSpin->value(),
                           m_triggerPositionSpin->value() / 100.0);
        m_view->setCursors(m_cursorsCheck->isChecked(), m_cursorChannelCombo->currentIndex());
    }

    void SignalMonitorWidget::startAcquisition()
    {
        QStringList sources;
        QHash<QString, int> firstChannel;
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            const QString sourceId = channelSource(index);
            if (m_channels[index].box->isChecked() && !sourceId.isEmpty() && !sources.contains(sourceId))
            {
                sources.append(sourceId);
                firstChannel.insert(sourceId, index);
            }
        }
        if (sources.isEmpty())
        {
            m_statusLabel->setText(tr("Turn on a channel that shows a source"));
            return;
        }
        saveSettings();

        const bool scanImage = m_scanGroup->isChecked() && m_channels[0].box->isChecked();
        m_scanSourceId = scanImage ? channelSource(0) : QString();
        m_scanLayerShown = false;
        m_sourceMessages.clear();
        m_view->clear();
        m_view->setRunning(true);

        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (const QString& sourceId : sources)
        {
            scopeone::core::SignalAcquisitionConfig config;
            config.sourceId = sourceId;
            config.sampleIntervalSeconds = m_sampleIntervalSpin->value() / 1000.0;
            config.scanImage.enabled = sourceId == m_scanSourceId;
            config.scanImage.width = m_scanWidthSpin->value();
            config.scanImage.height = m_scanHeightSpin->value();
            config.scanImage.gain = static_cast<quint32>(m_scanGainSpin->value());
            config.scanImage.averageFrames = m_scanAverageFramesSpin->value();
            config.scanImage.frameStartMarker = m_frameStartMarkerCombo->currentData().toUInt();
            config.scanImage.lineMarker = m_lineMarkerCombo->currentData().toUInt();
            config.scanImage.frameEndMarker = m_frameEndMarkerCombo->currentData().toUInt();
            config.scanImage.serpentine = m_serpentineCheck->isChecked();
            config.scanImage.mirrorHorizontal = m_mirrorHorizontalCheck->isChecked();
            config.sourceSettings = sourceSettings(firstChannel.value(sourceId));
            for (auto it = config.sourceSettings.constBegin(); it != config.sourceSettings.constEnd(); ++it)
            {
                settings.setValue(parameterSettingKey(sourceId, it.key()), it.value());
            }

            QString errorMessage;
            if (!m_core->startSignalTrace(config, &errorMessage))
            {
                qWarning().noquote() << errorMessage;
                stopAcquisition();
                m_statusLabel->setText(errorMessage);
                return;
            }
            if (!m_activeSources.contains(sourceId))
            {
                m_activeSources.append(sourceId);
            }
        }
        updateControls();
    }

    void SignalMonitorWidget::stopAcquisition()
    {
        for (const QString& sourceId : QStringList(m_activeSources))
        {
            m_core->stopSignalTrace(sourceId);
        }
    }

    // Fits each shown channel into six of the eight divisions around the centre and puts the
    // trigger level in the middle of its channel when it is outside the signal
    void SignalMonitorWidget::autoScale()
    {
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            ChannelControls& channel = m_channels[index];
            double minimum = 0.0;
            double maximum = 0.0;
            if (!channel.box->isChecked() || !m_view->displayedRange(index, &minimum, &maximum))
            {
                continue;
            }
            const double span = maximum > minimum ? maximum - minimum : std::max(std::abs(maximum), 1e-3);
            int step = channel.scale->count() - 1;
            for (int candidate = 0; candidate < channel.scale->count(); ++candidate)
            {
                if (channel.scale->itemData(candidate).toDouble() * 6.0 >= span)
                {
                    step = candidate;
                    break;
                }
            }
            const QSignalBlocker scaleBlocker(channel.scale);
            const QSignalBlocker offsetBlocker(channel.offset);
            channel.scale->setCurrentIndex(step);
            channel.offset->setValue(channel.coupling->currentData().toBool() ? 0.0 : -(minimum + maximum) / 2.0);
            if (index == m_triggerChannelCombo->currentIndex()
                && (m_triggerLevelSpin->value() <= minimum || m_triggerLevelSpin->value() >= maximum))
            {
                const QSignalBlocker levelBlocker(m_triggerLevelSpin);
                m_triggerLevelSpin->setValue((minimum + maximum) / 2.0);
            }
        }
        applyView();
    }

    void SignalMonitorWidget::handleTimeSeries(const scopeone::core::TimeSeriesChunk& chunk)
    {
        if (chunk.isValid())
        {
            m_view->appendChunk(chunk);
        }
    }

    void SignalMonitorWidget::handleStateChanged(const QString& sourceId,
                                                 scopeone::core::SignalSourceState state,
                                                 const QString& message)
    {
        bool shown = m_activeSources.contains(sourceId);
        for (int index = 0; index < OscilloscopeView::kChannelCount && !shown; ++index)
        {
            shown = channelSource(index) == sourceId;
        }
        if (!shown)
        {
            return;
        }
        m_sourceMessages.insert(sourceId, message);
        const bool active = state == scopeone::core::SignalSourceState::Starting
            || state == scopeone::core::SignalSourceState::Running
            || state == scopeone::core::SignalSourceState::Stopping;
        if (active && !m_activeSources.contains(sourceId))
        {
            m_activeSources.append(sourceId);
        }
        else if (!active)
        {
            m_activeSources.removeAll(sourceId);
        }
        updateControls();
        updateStatus();
    }

    // The sources and their settings are fixed while they run; the screen stays adjustable
    void SignalMonitorWidget::updateControls()
    {
        const bool running = !m_activeSources.isEmpty();
        bool startable = false;
        for (const ChannelControls& channel : m_channels)
        {
            channel.source->setEnabled(!running);
            for (QWidget* editor : channel.editors)
            {
                // A file parameter sits in a row with its browse button
                QWidget* const row = editor->parentWidget();
                (row && row != channel.box ? row : editor)->setEnabled(!running);
            }
            startable = startable || (channel.box->isChecked() && channel.source->currentIndex() >= 0);
        }
        m_sampleIntervalSpin->setEnabled(!running);
        m_scanGroup->setEnabled(!running);
        m_startButton->setEnabled(!running && startable);
        m_stopButton->setEnabled(running);
    }

    void SignalMonitorWidget::updateStatus()
    {
        if (m_sources.isEmpty())
        {
            m_statusLabel->setText(tr("No signal sources available"));
            return;
        }
        QStringList lines;
        for (auto it = m_sourceMessages.constBegin(); it != m_sourceMessages.constEnd(); ++it)
        {
            const QString name = descriptor(it.key()).name;
            lines.append(QStringLiteral("%1: %2").arg(name.isEmpty() ? it.key() : name, it.value()));
        }
        lines.sort();
        m_statusLabel->setText(lines.isEmpty() ? tr("Idle") : lines.join(QLatin1Char('\n')));
    }

    void SignalMonitorWidget::loadSettings()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        m_sampleIntervalSpin->setValue(settings.value(monitorKey(QStringLiteral("intervalMs")), 1.0).toDouble());
        selectNearest(m_timebaseCombo, settings.value(monitorKey(QStringLiteral("timebase")), 1e-3).toDouble());
        m_triggerPositionSpin->setValue(settings.value(monitorKey(QStringLiteral("triggerPosition")), 50.0).toDouble());
        m_triggerModeCombo->setCurrentIndex(std::max(0, m_triggerModeCombo->findData(
            settings.value(monitorKey(QStringLiteral("triggerMode")), static_cast<int>(ScopeTriggerMode::Auto)))));
        m_triggerChannelCombo->setCurrentIndex(
            std::clamp(settings.value(monitorKey(QStringLiteral("triggerChannel")), 0).toInt(),
                       0, OscilloscopeView::kChannelCount - 1));
        m_triggerSlopeCombo->setCurrentIndex(std::max(0, m_triggerSlopeCombo->findData(
            settings.value(monitorKey(QStringLiteral("triggerSlope")), static_cast<int>(ScopeTriggerSlope::Rising)))));
        m_triggerLevelSpin->setValue(settings.value(monitorKey(QStringLiteral("triggerLevel")), 0.0).toDouble());
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            ChannelControls& channel = m_channels[index];
            channel.box->setChecked(settings.value(channelKey(index, QStringLiteral("enabled")), index == 0).toBool());
            channel.preferredSource = settings.value(channelKey(index, QStringLiteral("source"))).toString();
            selectNearest(channel.scale, settings.value(channelKey(index, QStringLiteral("scale")), 1.0).toDouble());
            channel.offset->setValue(settings.value(channelKey(index, QStringLiteral("offset")), 0.0).toDouble());
            channel.coupling->setCurrentIndex(
                settings.value(channelKey(index, QStringLiteral("acCoupled")), false).toBool() ? 1 : 0);
        }
    }

    void SignalMonitorWidget::saveSettings() const
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        settings.setValue(monitorKey(QStringLiteral("intervalMs")), m_sampleIntervalSpin->value());
        settings.setValue(monitorKey(QStringLiteral("timebase")), m_timebaseCombo->currentData());
        settings.setValue(monitorKey(QStringLiteral("triggerPosition")), m_triggerPositionSpin->value());
        settings.setValue(monitorKey(QStringLiteral("triggerMode")), m_triggerModeCombo->currentData());
        settings.setValue(monitorKey(QStringLiteral("triggerChannel")), m_triggerChannelCombo->currentIndex());
        settings.setValue(monitorKey(QStringLiteral("triggerSlope")), m_triggerSlopeCombo->currentData());
        settings.setValue(monitorKey(QStringLiteral("triggerLevel")), m_triggerLevelSpin->value());
        for (int index = 0; index < OscilloscopeView::kChannelCount; ++index)
        {
            const ChannelControls& channel = m_channels[index];
            settings.setValue(channelKey(index, QStringLiteral("enabled")), channel.box->isChecked());
            settings.setValue(channelKey(index, QStringLiteral("source")), channel.preferredSource);
            settings.setValue(channelKey(index, QStringLiteral("scale")), channel.scale->currentData());
            settings.setValue(channelKey(index, QStringLiteral("offset")), channel.offset->value());
            settings.setValue(channelKey(index, QStringLiteral("acCoupled")), channel.coupling->currentData());
        }
    }

    scopeone::core::SignalSourceDescriptor SignalMonitorWidget::descriptor(const QString& sourceId) const
    {
        for (const auto& source : m_sources)
        {
            if (source.id == sourceId)
            {
                return source;
            }
        }
        return {};
    }

    QString SignalMonitorWidget::channelSource(int index) const
    {
        return m_channels[index].source->currentData().toString();
    }

    QVariantMap SignalMonitorWidget::sourceSettings(int index) const
    {
        QVariantMap values;
        const ChannelControls& channel = m_channels[index];
        for (auto it = channel.editors.constBegin(); it != channel.editors.constEnd(); ++it)
        {
            QWidget* const editor = it.value();
            if (const auto* spin = qobject_cast<QSpinBox*>(editor))
            {
                values.insert(it.key(), spin->value());
            }
            else if (const auto* spin = qobject_cast<QDoubleSpinBox*>(editor))
            {
                values.insert(it.key(), spin->value());
            }
            else if (const auto* combo = qobject_cast<QComboBox*>(editor))
            {
                values.insert(it.key(), combo->currentData());
            }
            else if (const auto* edit = qobject_cast<QLineEdit*>(editor))
            {
                values.insert(it.key(), edit->text().trimmed());
            }
        }
        return values;
    }
}

namespace scopeone::ui
{
    SCOPEONE_REGISTER_TOOL(
        {QStringLiteral("scopeone.signal_monitor"), QString::fromUtf8(QT_TRANSLATE_NOOP("Tools", "Signal Monitor")),
         QString::fromUtf8(QT_TRANSLATE_NOOP("Tools", "Acquisition")), ToolWindowMode::ModelessSingleton},
        [](ScopeOneToolContext& context, QWidget* parent)
        {
            return new SignalMonitorWidget(&context.core(), context, parent);
        })
}
