#include "RecordingWidget.h"
#include "CollapsibleSection.h"
#include "PositionListFile.h"
#include "scopeone/ScopeOneCore.h"
#include <QCheckBox>
#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QDebug>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    double intervalToMs(double value, const QString& unit)
    {
        if (unit.compare(QStringLiteral("ms"), Qt::CaseInsensitive) == 0) return value;
        if (unit.compare(QStringLiteral("s"), Qt::CaseInsensitive) == 0) return value * 1000.0;
        if (unit.compare(QStringLiteral("min"), Qt::CaseInsensitive) == 0) return value * 60000.0;
        if (unit.compare(QStringLiteral("h"), Qt::CaseInsensitive) == 0) return value * 3600000.0;
        return value;
    }

    QString phaseText(int phase)
    {
        switch (phase)
        {
        case scopeone::core::kRecordingPhaseIdle:
            return QStringLiteral("Idle");
        case scopeone::core::kRecordingPhaseRecording:
            return QStringLiteral("Recording...");
        case scopeone::core::kRecordingPhaseRecordingBurst:
            return QStringLiteral("Recording (burst mode)...");
        case scopeone::core::kRecordingPhaseRecordingMda:
            return QStringLiteral("Recording (MDA)...");
        case scopeone::core::kRecordingPhaseWaitingNextBurst:
            return QStringLiteral("Waiting for next burst...");
        case scopeone::core::kRecordingPhaseStopped:
            return QStringLiteral("Stopped");
        }
        return QStringLiteral("Idle");
    }

    QString formatStatusText(int phase, qint64 waitRemainingMs)
    {
        QString status = phaseText(phase);
        if (phase == scopeone::core::kRecordingPhaseWaitingNextBurst && waitRemainingMs > 0)
        {
            status += QString(" (%1 ms)").arg(waitRemainingMs);
        }

        return status;
    }

    QString formatMdaStatusText(int phase,
                                int mdaTimeIndex,
                                int mdaTimeCount,
                                int mdaZIndex,
                                int mdaZCount,
                                int mdaPositionIndex,
                                int mdaPositionCount,
                                bool hasXY,
                                double x,
                                double y,
                                bool hasZ,
                                double z)
    {
        if (phase != scopeone::core::kRecordingPhaseRecordingMda || mdaTimeIndex <= 0)
        {
            return {};
        }

        QStringList axes;
        axes.append(QString("T %1/%2").arg(mdaTimeIndex).arg((std::max)(1, mdaTimeCount)));
        if (mdaZCount > 1 && mdaZIndex > 0)
        {
            axes.append(QString("Z %1/%2").arg(mdaZIndex).arg(mdaZCount));
        }
        if (mdaPositionCount > 1 && mdaPositionIndex > 0)
        {
            axes.append(QString("XY %1/%2").arg(mdaPositionIndex).arg(mdaPositionCount));
        }

        QStringList position;
        if (hasXY)
        {
            position.append(QString("X %1  Y %2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3));
        }
        if (hasZ)
        {
            position.append(QString("Z %1").arg(z, 0, 'f', 3));
        }

        const QString axisText = axes.join(QStringLiteral("  "));
        return position.isEmpty()
                   ? axisText
                   : QStringLiteral("%1 | %2").arg(axisText, position.join(QStringLiteral("  ")));
    }

    QString formatFramesText(qint64 frameCurrent, qint64 frameTarget)
    {
        const qint64 target = (std::max)(0ll, frameTarget);
        return QString("%1 / %2 frames").arg(frameCurrent).arg(target);
    }

    // Formats a duration as the largest two units, such as 1 h 05 min
    QString formatDuration(double milliseconds)
    {
        const qint64 seconds = static_cast<qint64>(milliseconds / 1000.0 + 0.5);
        if (seconds < 1)
        {
            return QStringLiteral("under 1 s");
        }
        if (seconds < 60)
        {
            return QStringLiteral("%1 s").arg(seconds);
        }
        if (seconds < 3600)
        {
            return QStringLiteral("%1 min %2 s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1 h %2 min").arg(seconds / 3600).arg(seconds % 3600 / 60, 2, 10, QLatin1Char('0'));
    }

    QString formatBurstsText(int burstCurrent, int burstTarget)
    {
        if (burstTarget <= 0)
        {
            return {};
        }
        return QString("Burst %1 / %2").arg(burstCurrent).arg(burstTarget);
    }

    QString formatByteCount(qint64 bytes)
    {
        const qint64 clamped = (std::max)(0ll, bytes);
        static const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
        double value = static_cast<double>(clamped);
        int suffixIndex = 0;
        while (value >= 1024.0 && suffixIndex < 4)
        {
            value /= 1024.0;
            ++suffixIndex;
        }
        const int decimals = (suffixIndex == 0 || value >= 100.0) ? 0 : (value >= 10.0 ? 1 : 2);
        return QString("%1 %2")
               .arg(value, 0, 'f', decimals)
               .arg(QString::fromLatin1(suffixes[suffixIndex]));
    }

    QString writerPhaseText(scopeone::core::ScopeOneCore::RecordingWriterPhase phase)
    {
        using RecordingWriterPhase = scopeone::core::ScopeOneCore::RecordingWriterPhase;
        switch (phase)
        {
        case RecordingWriterPhase::Idle:
            return QStringLiteral("Idle");
        case RecordingWriterPhase::Starting:
            return QStringLiteral("Starting");
        case RecordingWriterPhase::Writing:
            return QStringLiteral("Writing");
        case RecordingWriterPhase::Stopping:
            return QStringLiteral("Finalizing");
        case RecordingWriterPhase::Completed:
            return QStringLiteral("Completed");
        case RecordingWriterPhase::Failed:
            return QStringLiteral("Failed");
        }
        return QStringLiteral("Idle");
    }

    QString formatWriterStatusText(const scopeone::core::ScopeOneCore::RecordingWriterStatus& status)
    {
        QString text = QStringLiteral("Disk: %1").arg(writerPhaseText(status.phase()));
        QStringList details;
        if (status.framesWritten() > 0 || status.phase() ==
            scopeone::core::ScopeOneCore::RecordingWriterPhase::Completed)
        {
            details.append(QStringLiteral("%1 frames written").arg(status.framesWritten()));
        }
        if (status.droppedFrames() > 0)
        {
            details.append(QStringLiteral("%1 dropped").arg(status.droppedFrames()));
        }
        if (status.bytesWritten() > 0)
        {
            details.append(QStringLiteral("%1 data").arg(formatByteCount(status.bytesWritten())));
        }
        if (status.maxPendingWriteBytes() > 0)
        {
            details.append(QStringLiteral("Queue %1 / %2")
                           .arg(formatByteCount(status.pendingWriteBytes()))
                           .arg(formatByteCount(status.maxPendingWriteBytes())));
        }
        else if (status.pendingWriteBytes() > 0)
        {
            details.append(QStringLiteral("Queue %1").arg(formatByteCount(status.pendingWriteBytes())));
        }
        if (!status.errorMessage().isEmpty())
        {
            details.append(status.errorMessage());
        }
        return details.isEmpty() ? text : QStringLiteral("%1 - %2").arg(text, details.join(QStringLiteral(", ")));
    }

} // namespace

namespace scopeone::ui
{
    RecordingWidget::RecordingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("RecordingWidget requires ScopeOneCore");
        }

        setupUI();
        connect(m_browseButton, &QPushButton::clicked, this, &RecordingWidget::onBrowseClicked);
        connect(m_autoNameButton, &QPushButton::clicked, this, &RecordingWidget::onAutoNameClicked);
        connect(m_startStopButton, &QPushButton::clicked, this, &RecordingWidget::onStartStopClicked);
        connect(m_burstModeCheck, &QCheckBox::toggled, this, [this]() { updateUiState(); });
        connect(m_detectorCombo, &QComboBox::currentTextChanged, this, [this]() { updateUiState(); });
        connect(m_saveDirLineEdit, &QLineEdit::textChanged, this,
                [this]()
                {
                    setLastSaveDirectory(m_saveDirLineEdit->text().trimmed());
                    updateUiState();
                });
        connect(m_fileNameLineEdit, &QLineEdit::textChanged, this, [this]() { updateUiState(); });
        connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this]() { updateUiState(); });
        connect(m_compressionCheck, &QCheckBox::toggled, this, [this]() { updateUiState(); });
        connect(m_framesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this]() { updateStorageStatus(); });
        connect(m_burstCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this]() { updateStorageStatus(); });
        connect(m_mdaZCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this]() { updateStorageStatus(); });
        connect(m_mdaXCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this]() { updateStorageStatus(); });
        connect(m_mdaYCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this]() { updateStorageStatus(); });
        m_storageStatusTimer = new QTimer(this);
        m_storageStatusTimer->setInterval(1000);
        connect(m_storageStatusTimer, &QTimer::timeout, this, &RecordingWidget::updateStorageStatus);
        connect(m_mdaEnableZCheck, &QCheckBox::toggled, this, [this]()
        {
            syncOrderList();
            updateUiState();
        });
        connect(m_mdaEnableXYCheck, &QCheckBox::toggled, this, [this]()
        {
            syncOrderList();
            updateUiState();
        });
        connect(m_mdaAutofocusCheck, &QCheckBox::toggled, this, [this]() { updateUiState(); });
        connect(m_mdaEnableChannelCheck, &QCheckBox::toggled, this, [this](bool enabled)
        {
            if (enabled)
            {
                refreshChannelGroups();
            }
            syncOrderList();
            updateUiState();
        });
        connect(m_channelGroupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this]() { loadChannelPresets(); });
        connect(m_channelTable, &QTableWidget::itemChanged, this, [this]()
        {
            syncOrderList();
            updateUiState();
        });
        connect(m_channelUpButton, &QPushButton::clicked, this, [this]() { moveChannelRow(-1); });
        connect(m_channelDownButton, &QPushButton::clicked, this, [this]() { moveChannelRow(1); });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::hardwareConfigurationChanged, this,
                [this]() { refreshChannelGroups(); });
        connect(m_mdaOrderList, &QListWidget::currentRowChanged, this, [this]() { updateUiState(); });
        connect(m_mdaOrderUpButton, &QPushButton::clicked, this, [this]() { moveOrderItem(-1); });
        connect(m_mdaOrderDownButton, &QPushButton::clicked, this, [this]() { moveOrderItem(1); });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingProgressChanged, this,
                [this](int phase,
                       qint64 frameCurrent,
                       qint64 frameTarget,
                       int burstCurrent,
                       int burstTarget,
                       qint64 waitRemainingMs,
                       int mdaTimeIndex,
                       int mdaTimeCount,
                       int mdaZIndex,
                       int mdaZCount,
                       int mdaPositionIndex,
                       int mdaPositionCount,
                       bool hasXY,
                       double x,
                       double y,
                       bool hasZ,
                       double z)
                {
                    m_statusLabel->setText(formatStatusText(phase, waitRemainingMs));
                    const QString mdaStatus = formatMdaStatusText(phase,
                                                                 mdaTimeIndex,
                                                                 mdaTimeCount,
                                                                 mdaZIndex,
                                                                 mdaZCount,
                                                                 mdaPositionIndex,
                                                                 mdaPositionCount,
                                                                 hasXY,
                                                                 x,
                                                                 y,
                                                                 hasZ,
                                                                 z);
                    m_mdaStatusLabel->setText(mdaStatus);
                    m_mdaStatusLabel->setVisible(!mdaStatus.isEmpty());
                    m_frameCountLabel->setText(formatFramesText(frameCurrent, frameTarget));
                    const bool progressVisible = phase != scopeone::core::kRecordingPhaseIdle
                        && phase != scopeone::core::kRecordingPhaseStopped;
                    m_frameCountLabel->setVisible(progressVisible);
                    // Overall progress over all bursts, with a rough estimate of the time left
                    if (progressVisible && frameTarget > 0)
                    {
                        const qint64 bursts = (std::max)(1, burstTarget);
                        const qint64 total = frameTarget * bursts;
                        const qint64 done = (std::min)(total,
                                                       (burstCurrent > 0 ? (burstCurrent - 1) * frameTarget : 0)
                                                           + frameCurrent);
                        m_progressBar->setRange(0, static_cast<int>((std::min<qint64>)(total, 2000000000)));
                        m_progressBar->setValue(static_cast<int>((std::min<qint64>)(done, 2000000000)));
                        QString format = QStringLiteral("%p%");
                        if (done > 0 && done < total && m_runTimer.isValid() && m_runTimer.elapsed() > 2000)
                        {
                            format += QStringLiteral(" · about %1 left")
                                          .arg(formatDuration(static_cast<double>(m_runTimer.elapsed())
                                                              / static_cast<double>(done)
                                                              * static_cast<double>(total - done)));
                        }
                        m_progressBar->setFormat(format);
                        m_progressBar->show();
                    }
                    else if (!progressVisible)
                    {
                        m_progressBar->hide();
                    }
                    const QString burstStatus = formatBurstsText(burstCurrent, burstTarget);
                    m_burstCountLabel->setText(burstStatus);
                    m_burstCountLabel->setVisible(progressVisible && !burstStatus.isEmpty());
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStateChanged, this,
                [this](bool recording)
                {
                    m_isRecording = recording;
                    if (recording)
                    {
                        m_runTimer.start();
                        m_failureLabel->hide();
                    }
                    else
                    {
                        m_progressBar->hide();
                    }
                    m_startStopButton->setText(recording ? "Stop" : "Start");
                    m_startStopButton->setStyleSheet(recording
                                                         ? QStringLiteral(
                                                             "QPushButton { background-color: #c0392b; color: white; font-weight: bold; }")
                                                         : QString());
                    if (recording)
                    {
                        m_storageStatusTimer->start();
                    }
                    else
                    {
                        m_storageStatusTimer->stop();
                    }
                    updateUiState();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingWriterStatusChanged, this,
                [this](const scopeone::core::ScopeOneCore::RecordingWriterStatus& status)
                {
                    const bool writerFinalizing =
                        status.phase() == scopeone::core::ScopeOneCore::RecordingWriterPhase::Stopping;
                    m_writerStatusLabel->setText(formatWriterStatusText(status));
                    m_writerStatusLabel->setVisible(
                        status.phase() != scopeone::core::ScopeOneCore::RecordingWriterPhase::Idle);
                    if (m_writerFinalizing != writerFinalizing)
                    {
                        m_writerFinalizing = writerFinalizing;
                        updateUiState();
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped, this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    const QString result = session ? session->saveMessage() : QString();
                    const bool saved = session && session->isSaved();
                    // Say why a run failed where the user is looking
                    if (session
                        && session->experimentDocument().runState == scopeone::core::ExperimentRunState::Failed)
                    {
                        const QString reason = session->experimentDocument().errorMessage;
                        m_failureLabel->setText(QStringLiteral("The acquisition failed: %1")
                                                    .arg(reason.isEmpty() ? QStringLiteral("unknown error") : reason));
                        m_failureLabel->show();
                    }
                    if (result.isEmpty())
                    {
                        return;
                    }
                    if (saved)
                    {
                        qInfo().noquote() << result;
                    }
                    else
                    {
                        qWarning().noquote() << result;
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingSessionSaveFinished, this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    const QString result = session ? session->saveMessage() : QString();
                    const bool saved = session && session->isSaved();
                    if (saved)
                    {
                        m_writerStatusLabel->setText(
                            formatWriterStatusText(m_scopeonecore->recordingWriterStatus()));
                    }
                    else if (!result.isEmpty())
                    {
                        m_writerStatusLabel->setText(QStringLiteral("Disk: Failed - %1").arg(result));
                    }
                    else
                    {
                        m_writerStatusLabel->setText(QStringLiteral("Disk: Failed - Error: no session data"));
                    }
                    m_writerStatusLabel->show();
                    if (result.isEmpty())
                    {
                        qWarning().noquote() << "Error: no session data";
                        return;
                    }
                    if (saved)
                    {
                        qInfo().noquote() << result;
                    }
                    else
                    {
                        qWarning().noquote() << result;
                    }
                });

        if (m_saveDirLineEdit->text().trimmed().isEmpty())
        {
            m_saveDirLineEdit->setText(getLastSaveDirectory());
        }
        if (m_fileNameLineEdit->text().trimmed().isEmpty())
        {
            m_fileNameLineEdit->setText(buildTimestampBaseName());
        }

        restoreLastSettings();
        updateUiState();
    }

    RecordingWidget::~RecordingWidget()
    {
        saveLastSettings();
        m_scopeonecore->stopRecording();
    }

    // Builds the recording control panel
    void RecordingWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        auto* scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setFrameShape(QFrame::NoFrame);

        auto* content = new QWidget(scrollArea);
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(6, 6, 6, 6);
        contentLayout->setSpacing(6);

        auto* captureGroup = new QGroupBox("Capture", this);
        auto* captureLayout = new QGridLayout(captureGroup);
        captureLayout->setHorizontalSpacing(6);
        captureLayout->setVerticalSpacing(4);

        m_detectorCombo = new QComboBox(this);
        m_detectorCombo->addItem("All");
        captureLayout->addWidget(new QLabel("Detector:", this), 0, 0);
        captureLayout->addWidget(m_detectorCombo, 0, 1, 1, 2);

        m_saveDirLineEdit = new QLineEdit(this);
        m_browseButton = new QPushButton("Browse", this);
        captureLayout->addWidget(new QLabel("Save Dir:", this), 1, 0);
        captureLayout->addWidget(m_saveDirLineEdit, 1, 1);
        captureLayout->addWidget(m_browseButton, 1, 2);

        m_fileNameLineEdit = new QLineEdit(this);
        m_autoNameButton = new QPushButton("Auto", this);
        captureLayout->addWidget(new QLabel("File Name:", this), 2, 0);
        captureLayout->addWidget(m_fileNameLineEdit, 2, 1);
        captureLayout->addWidget(m_autoNameButton, 2, 2);

        contentLayout->addWidget(captureGroup);

        auto* formatGroup = new QGroupBox("Format", this);
        auto* formatLayout = new QGridLayout(formatGroup);
        formatLayout->setHorizontalSpacing(6);
        formatLayout->setVerticalSpacing(4);

        m_formatCombo = new QComboBox(this);
        m_formatCombo->addItem("OME-TIFF", static_cast<int>(scopeone::core::RecordingFormat::OmeTiff));
        m_formatCombo->addItem("OME-Zarr", static_cast<int>(scopeone::core::RecordingFormat::OmeZarr));
        m_formatCombo->addItem("TIFF", static_cast<int>(scopeone::core::RecordingFormat::Tiff));
        m_formatCombo->addItem("Binary", static_cast<int>(scopeone::core::RecordingFormat::Binary));
        formatLayout->addWidget(new QLabel("Raw Format:", this), 0, 0);
        formatLayout->addWidget(m_formatCombo, 0, 1, 1, 2);

        m_compressionCheck = new QCheckBox("Compression", this);
        m_compressionCheck->setChecked(false);
        m_compressionLevelSpin = new QSpinBox(this);
        m_compressionLevelSpin->setRange(0, 9);
        m_compressionLevelSpin->setValue(6);
        m_compressionLevelSpin->setMaximumWidth(60);
        formatLayout->addWidget(m_compressionCheck, 1, 0);
        formatLayout->addWidget(new QLabel("Level:", this), 1, 1);
        formatLayout->addWidget(m_compressionLevelSpin, 1, 2);

        m_pyramidCheck = new QCheckBox("Resolution pyramid", this);
        m_pyramidCheck->setChecked(false);
        m_pyramidCheck->setToolTip(
            "OME-Zarr only. Adds half-resolution levels so large images open quickly "
            "in viewers such as napari. Uses about one third more disk space.");
        formatLayout->addWidget(m_pyramidCheck, 2, 0, 1, 3);

        contentLayout->addWidget(formatGroup);

        // The acquisition settings are split into blocks that fold away, each with a summary
        // of its settings in the title
        m_timeSection = new scopeone::gui::CollapsibleSection("Time", this);
        m_zSection = new scopeone::gui::CollapsibleSection("Z stack", this);
        m_positionSection = new scopeone::gui::CollapsibleSection("Positions", this);
        m_channelSection = new scopeone::gui::CollapsibleSection("Channels", this);
        m_advancedSection = new scopeone::gui::CollapsibleSection("Order and options", this);
        QFormLayout* mdaLayout = m_timeSection->form();

        m_framesSpin = new QSpinBox(this);
        m_framesSpin->setRange(1, 1000000);
        m_framesSpin->setValue(100);
        mdaLayout->addRow("Frames:", m_framesSpin);

        m_mdaIntervalSpin = new QDoubleSpinBox(this);
        m_mdaIntervalSpin->setRange(0.0, 1000000.0);
        m_mdaIntervalSpin->setDecimals(1);
        m_mdaIntervalSpin->setValue(0.0);
        mdaLayout->addRow("Interval (ms):", m_mdaIntervalSpin);

        mdaLayout = m_zSection->form();
        m_mdaEnableZCheck = new QCheckBox("Enable Z Stack", this);
        mdaLayout->addRow("", m_mdaEnableZCheck);

        // Size the axis spin boxes for typical values instead of the full +/-1e6 range
        const int mdaSpinWidth = (fontMetrics().horizontalAdvance(QStringLiteral("-0000.000 µm")) + 28) * 2 / 3;
        auto* zRowLayout = new QHBoxLayout();
        zRowLayout->setSpacing(6);
        m_mdaZStartSpin = new QDoubleSpinBox(this);
        m_mdaZStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaZStartSpin->setDecimals(3);
        m_mdaZStartSpin->setValue(0.0);
        m_mdaZStartSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaZStartSpin->setFixedWidth(mdaSpinWidth);
        m_mdaZStepSpin = new QDoubleSpinBox(this);
        m_mdaZStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaZStepSpin->setDecimals(3);
        m_mdaZStepSpin->setValue(1.0);
        m_mdaZStepSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaZStepSpin->setFixedWidth(mdaSpinWidth);
        m_mdaZCountSpin = new QSpinBox(this);
        m_mdaZCountSpin->setRange(1, 10000);
        m_mdaZCountSpin->setValue(1);
        zRowLayout->addWidget(new QLabel("Start", this));
        zRowLayout->addWidget(m_mdaZStartSpin);
        zRowLayout->addStretch();
        zRowLayout->addWidget(new QLabel("Step", this));
        zRowLayout->addWidget(m_mdaZStepSpin);
        zRowLayout->addStretch();
        zRowLayout->addWidget(new QLabel("Count", this));
        zRowLayout->addWidget(m_mdaZCountSpin);
        mdaLayout->addRow("Z:", zRowLayout);

        m_mdaZRelativeCheck = new QCheckBox("Relative to the current Z", this);
        m_mdaZRelativeCheck->setToolTip(QStringLiteral(
            "The Z start is measured from the focus position when the recording starts"));
        mdaLayout->addRow("", m_mdaZRelativeCheck);

        mdaLayout = m_positionSection->form();
        m_mdaEnableXYCheck = new QCheckBox("Enable XY positions", this);
        mdaLayout->addRow("", m_mdaEnableXYCheck);

        auto* xRowLayout = new QHBoxLayout();
        xRowLayout->setSpacing(6);
        m_mdaXStartSpin = new QDoubleSpinBox(this);
        m_mdaXStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaXStartSpin->setDecimals(3);
        m_mdaXStartSpin->setValue(0.0);
        m_mdaXStartSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaXStartSpin->setFixedWidth(mdaSpinWidth);
        m_mdaXStepSpin = new QDoubleSpinBox(this);
        m_mdaXStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaXStepSpin->setDecimals(3);
        m_mdaXStepSpin->setValue(1.0);
        m_mdaXStepSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaXStepSpin->setFixedWidth(mdaSpinWidth);
        m_mdaXCountSpin = new QSpinBox(this);
        m_mdaXCountSpin->setRange(1, 10000);
        m_mdaXCountSpin->setValue(1);
        xRowLayout->addWidget(new QLabel("Start", this));
        xRowLayout->addWidget(m_mdaXStartSpin);
        xRowLayout->addStretch();
        xRowLayout->addWidget(new QLabel("Step", this));
        xRowLayout->addWidget(m_mdaXStepSpin);
        xRowLayout->addStretch();
        xRowLayout->addWidget(new QLabel("Count", this));
        xRowLayout->addWidget(m_mdaXCountSpin);
        mdaLayout->addRow("X:", xRowLayout);

        auto* yRowLayout = new QHBoxLayout();
        yRowLayout->setSpacing(6);
        m_mdaYStartSpin = new QDoubleSpinBox(this);
        m_mdaYStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaYStartSpin->setDecimals(3);
        m_mdaYStartSpin->setValue(0.0);
        m_mdaYStartSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaYStartSpin->setFixedWidth(mdaSpinWidth);
        m_mdaYStepSpin = new QDoubleSpinBox(this);
        m_mdaYStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaYStepSpin->setDecimals(3);
        m_mdaYStepSpin->setValue(1.0);
        m_mdaYStepSpin->setSuffix(QStringLiteral(" µm"));
        m_mdaYStepSpin->setFixedWidth(mdaSpinWidth);
        m_mdaYCountSpin = new QSpinBox(this);
        m_mdaYCountSpin->setRange(1, 10000);
        m_mdaYCountSpin->setValue(1);
        yRowLayout->addWidget(new QLabel("Start", this));
        yRowLayout->addWidget(m_mdaYStartSpin);
        yRowLayout->addStretch();
        yRowLayout->addWidget(new QLabel("Step", this));
        yRowLayout->addWidget(m_mdaYStepSpin);
        yRowLayout->addStretch();
        yRowLayout->addWidget(new QLabel("Count", this));
        yRowLayout->addWidget(m_mdaYCountSpin);
        mdaLayout->addRow("Y:", yRowLayout);

        // A list of stage positions, like the position list of Micro-Manager. When it has
        // entries it is used instead of the grid.
        m_positionTable = new QTableWidget(0, 4, this);
        m_positionTable->setHorizontalHeaderLabels({"Label", "X (µm)", "Y (µm)", "Z (µm)"});
        m_positionTable->horizontalHeader()->setStretchLastSection(true);
        m_positionTable->verticalHeader()->setVisible(false);
        m_positionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_positionTable->setFixedHeight(120);
        m_positionTable->setToolTip(
            "Stage positions to image. When the list has entries it is used instead of the grid. "
            "An empty Z keeps the focus where it is.");
        m_positionMarkButton = new QPushButton("Mark", this);
        m_positionMarkButton->setToolTip("Add the current stage position");
        m_positionRemoveButton = new QPushButton("Remove", this);
        m_positionClearButton = new QPushButton("Clear", this);
        m_positionFromGridButton = new QPushButton("From grid", this);
        m_positionFromGridButton->setToolTip("Fill the list with the positions of the grid");
        m_positionLoadButton = new QPushButton("Load…", this);
        m_positionSaveButton = new QPushButton("Save…", this);
        auto* positionButtons = new QVBoxLayout();
        for (QPushButton* button : {m_positionMarkButton, m_positionRemoveButton, m_positionClearButton,
                                    m_positionFromGridButton, m_positionLoadButton, m_positionSaveButton})
        {
            button->setMaximumWidth(80);
            positionButtons->addWidget(button);
        }
        positionButtons->addStretch();
        auto* positionLayout = new QHBoxLayout();
        positionLayout->addWidget(m_positionTable, 1);
        positionLayout->addLayout(positionButtons);
        mdaLayout->addRow("Positions:", positionLayout);
        connect(m_positionMarkButton, &QPushButton::clicked, this, [this]()
        {
            double x = 0.0;
            double y = 0.0;
            if (!m_scopeonecore->readXYPosition(m_scopeonecore->currentXYStageDevice(), x, y))
            {
                qWarning().noquote() << "Cannot read the XY position";
                return;
            }
            double z = 0.0;
            const bool hasZ = m_scopeonecore->readZPosition(m_scopeonecore->currentFocusDevice(), z);
            addListedPosition(QStringLiteral("Pos%1").arg(m_positionTable->rowCount() + 1),
                              x,
                              y,
                              hasZ ? QString::number(z, 'f', 3) : QString());
            updateUiState();
        });
        connect(m_positionRemoveButton, &QPushButton::clicked, this, [this]()
        {
            const auto rows = m_positionTable->selectionModel()->selectedRows();
            QList<int> indexes;
            for (const QModelIndex& index : rows)
            {
                indexes.push_back(index.row());
            }
            std::sort(indexes.begin(), indexes.end(), std::greater<int>());
            for (int row : indexes)
            {
                m_positionTable->removeRow(row);
            }
            updateUiState();
        });
        connect(m_positionClearButton, &QPushButton::clicked, this, [this]()
        {
            m_positionTable->setRowCount(0);
            updateUiState();
        });
        connect(m_positionFromGridButton, &QPushButton::clicked, this, [this]() { fillPositionListFromGrid(); });
        connect(m_positionLoadButton, &QPushButton::clicked, this, [this]() { loadPositionList(); });
        connect(m_positionSaveButton, &QPushButton::clicked, this, [this]() { savePositionList(); });

        auto* autofocusLayout = new QHBoxLayout();
        m_mdaAutofocusCheck = new QCheckBox("Autofocus", this);
        m_mdaAutofocusCheck->setToolTip(QStringLiteral(
            "Runs the autofocus device of Micro-Manager before the first image of each position"));
        m_mdaAutofocusEverySpin = new QSpinBox(this);
        m_mdaAutofocusEverySpin->setRange(1, 100000);
        m_mdaAutofocusEverySpin->setValue(1);
        m_mdaAutofocusEverySpin->setPrefix(QStringLiteral("every "));
        m_mdaAutofocusEverySpin->setSuffix(QStringLiteral(" time point(s)"));
        autofocusLayout->addWidget(m_mdaAutofocusCheck);
        autofocusLayout->addWidget(m_mdaAutofocusEverySpin);
        autofocusLayout->addStretch();
        mdaLayout->addRow("", autofocusLayout);

        mdaLayout = m_channelSection->form();
        m_mdaEnableChannelCheck = new QCheckBox("Enable Channels", this);
        mdaLayout->addRow("", m_mdaEnableChannelCheck);

        m_channelGroupCombo = new QComboBox(this);
        m_channelGroupCombo->setToolTip(QStringLiteral(
            "Configuration group whose presets are the channels"));
        mdaLayout->addRow("Group:", m_channelGroupCombo);

        m_channelTable = new QTableWidget(0, 8, this);
        m_channelTable->setHorizontalHeaderLabels(
            {"Use", "Preset", "Exposure (ms)", "Z offset (µm)", "Z stack", "Skip", "Camera", "Color"});
        m_channelTable->horizontalHeaderItem(6)->setToolTip(
            QStringLiteral("Camera that images this channel. All uses every selected camera."));
        m_channelTable->horizontalHeaderItem(2)->setToolTip(
            QStringLiteral("Exposure of this channel. 0 uses the exposure of the camera."));
        m_channelTable->horizontalHeaderItem(3)->setToolTip(
            QStringLiteral("Focus offset of this channel"));
        m_channelTable->horizontalHeaderItem(4)->setToolTip(
            QStringLiteral("Unchecked images this channel only at the middle Z position"));
        m_channelTable->horizontalHeaderItem(5)->setToolTip(
            QStringLiteral("Number of time points skipped between images of this channel"));
        m_channelTable->verticalHeader()->hide();
        m_channelTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_channelTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_channelTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_channelTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_channelTable->setFixedHeight(130);

        m_channelUpButton = new QPushButton("Up", this);
        m_channelDownButton = new QPushButton("Down", this);
        m_channelUpButton->setMaximumWidth(70);
        m_channelDownButton->setMaximumWidth(70);
        auto* channelButtonsLayout = new QVBoxLayout();
        channelButtonsLayout->addWidget(m_channelUpButton);
        channelButtonsLayout->addWidget(m_channelDownButton);
        channelButtonsLayout->addStretch();
        auto* channelLayout = new QHBoxLayout();
        channelLayout->addWidget(m_channelTable, 1);
        channelLayout->addLayout(channelButtonsLayout);
        mdaLayout->addRow("Channels:", channelLayout);

        mdaLayout = m_advancedSection->form();
        m_keepShutterSlicesCheck = new QCheckBox("Keep the shutter open between Z positions", this);
        m_keepShutterChannelsCheck = new QCheckBox("Keep the shutter open between channels", this);
        mdaLayout->addRow("", m_keepShutterSlicesCheck);
        mdaLayout->addRow("", m_keepShutterChannelsCheck);
        m_hardwareSequencingCheck = new QCheckBox("Use hardware sequencing for Z stacks", this);
        m_hardwareSequencingCheck->setToolTip(
            "Let the focus device step through the Z positions on the camera triggers. "
            "Used where the focus device and the camera support sequences.");
        mdaLayout->addRow("", m_hardwareSequencingCheck);
        m_commentLineEdit = new QLineEdit(this);
        m_commentLineEdit->setPlaceholderText("Stored with the acquisition");
        mdaLayout->addRow("Comment:", m_commentLineEdit);

        m_mdaOrderList = new QListWidget(this);
        m_mdaOrderList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_mdaOrderList->setFixedHeight(72);
        m_orderPreference = {
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time),
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Z),
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::XY),
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Channel)
        };
        syncOrderList();

        auto* orderButtonsLayout = new QVBoxLayout();
        m_mdaOrderUpButton = new QPushButton("Up", this);
        m_mdaOrderDownButton = new QPushButton("Down", this);
        m_mdaOrderUpButton->setMaximumWidth(70);
        m_mdaOrderDownButton->setMaximumWidth(70);
        orderButtonsLayout->addWidget(m_mdaOrderUpButton);
        orderButtonsLayout->addWidget(m_mdaOrderDownButton);
        orderButtonsLayout->addStretch();

        auto* orderLayout = new QHBoxLayout();
        orderLayout->addWidget(m_mdaOrderList, 1);
        orderLayout->addLayout(orderButtonsLayout);
        mdaLayout->addRow("Order", orderLayout);

        mdaLayout = m_timeSection->form();
        m_burstModeCheck = new QCheckBox("Burst mode", this);
        mdaLayout->addRow("", m_burstModeCheck);

        m_burstCountSpin = new QSpinBox(this);
        m_burstCountSpin->setRange(2, 100000);
        m_burstCountSpin->setValue(10);
        mdaLayout->addRow("Bursts:", m_burstCountSpin);

        auto* burstIntervalLayout = new QHBoxLayout();
        m_burstIntervalSpin = new QDoubleSpinBox(this);
        m_burstIntervalSpin->setRange(0.1, 1000000.0);
        m_burstIntervalSpin->setDecimals(1);
        m_burstIntervalSpin->setValue(1.0);
        m_burstIntervalUnitCombo = new QComboBox(this);
        m_burstIntervalUnitCombo->addItems({"ms", "s", "min", "h"});
        m_burstIntervalUnitCombo->setCurrentIndex(1);
        m_burstIntervalUnitCombo->setMaximumWidth(60);
        burstIntervalLayout->addWidget(m_burstIntervalSpin);
        burstIntervalLayout->addWidget(m_burstIntervalUnitCombo);
        burstIntervalLayout->addStretch();
        mdaLayout->addRow("Burst Interval:", burstIntervalLayout);

        contentLayout->addWidget(m_timeSection);
        contentLayout->addWidget(m_zSection);
        contentLayout->addWidget(m_positionSection);
        contentLayout->addWidget(m_channelSection);
        contentLayout->addWidget(m_advancedSection);
        // Only the time settings are always needed. The others open with their checkbox.
        for (auto* section : {m_zSection, m_positionSection, m_channelSection, m_advancedSection})
        {
            section->setExpanded(false);
        }
        const auto openWith = [](QCheckBox* check, scopeone::gui::CollapsibleSection* section)
        {
            connect(check, &QCheckBox::toggled, section, [section](bool on)
            {
                if (on)
                {
                    section->setExpanded(true);
                }
            });
        };
        openWith(m_mdaEnableZCheck, m_zSection);
        openWith(m_mdaEnableXYCheck, m_positionSection);
        openWith(m_mdaEnableChannelCheck, m_channelSection);
        // The summaries follow the settings
        const auto refresh = [this]() { updateSectionSummaries(); updatePlanSummary(); };
        for (QSpinBox* spin : {m_framesSpin, m_mdaZCountSpin, m_mdaXCountSpin, m_mdaYCountSpin,
                               m_burstCountSpin, m_mdaAutofocusEverySpin})
        {
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, refresh);
        }
        for (QDoubleSpinBox* spin : {m_mdaIntervalSpin, m_mdaZStepSpin, m_burstIntervalSpin})
        {
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, refresh);
        }
        for (QCheckBox* check : {m_mdaEnableZCheck, m_mdaEnableXYCheck, m_mdaEnableChannelCheck,
                                 m_mdaAutofocusCheck, m_burstModeCheck, m_keepShutterSlicesCheck,
                                 m_keepShutterChannelsCheck, m_hardwareSequencingCheck, m_mdaZRelativeCheck})
        {
            connect(check, &QCheckBox::toggled, this, refresh);
        }
        connect(m_channelTable, &QTableWidget::itemChanged, this, refresh);
        connect(m_positionTable->model(), &QAbstractItemModel::rowsInserted, this, refresh);
        connect(m_positionTable->model(), &QAbstractItemModel::rowsRemoved, this, refresh);
        updateSectionSummaries();

        auto* statusGroup = new QGroupBox("Status", this);
        auto* statusLayout = new QGridLayout(statusGroup);
        statusLayout->setHorizontalSpacing(8);
        statusLayout->setVerticalSpacing(2);
        statusLayout->setColumnStretch(0, 1);
        m_statusLabel = new QLabel("Idle", this);
        m_mdaStatusLabel = new QLabel(this);
        m_mdaStatusLabel->hide();
        m_writerStatusLabel = new QLabel(this);
        m_writerStatusLabel->hide();
        m_frameCountLabel = new QLabel(this);
        m_frameCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_frameCountLabel->hide();
        m_burstCountLabel = new QLabel(this);
        m_burstCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_burstCountLabel->hide();
        m_storageStatusLabel = new QLabel("Storage: unavailable", this);
        for (QLabel* label : {m_mdaStatusLabel, m_writerStatusLabel, m_storageStatusLabel})
        {
            label->setWordWrap(true);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        }
        statusLayout->addWidget(m_statusLabel, 0, 0);
        statusLayout->addWidget(m_frameCountLabel, 0, 1);
        statusLayout->addWidget(m_mdaStatusLabel, 1, 0);
        statusLayout->addWidget(m_burstCountLabel, 1, 1);
        statusLayout->addWidget(m_writerStatusLabel, 2, 0, 1, 2);
        statusLayout->addWidget(m_storageStatusLabel, 3, 0, 1, 2);

        m_planSummaryLabel = new QLabel(this);
        m_planSummaryLabel->setWordWrap(true);
        m_planSummaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_planSummaryLabel->setToolTip(QStringLiteral(
            "What the settings above will acquire. The time counts exposures only; moving the "
            "stages, changing channels and writing the images take more."));
        {
            QFont font = m_planSummaryLabel->font();
            font.setBold(true);
            m_planSummaryLabel->setFont(font);
        }
        m_progressBar = new QProgressBar(this);
        m_progressBar->setTextVisible(true);
        m_progressBar->setRange(0, 1);
        m_progressBar->hide();
        m_failureLabel = new QLabel(this);
        m_failureLabel->setWordWrap(true);
        m_failureLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_failureLabel->setStyleSheet(QStringLiteral("QLabel { color: #c0392b; font-weight: bold; }"));
        m_failureLabel->hide();
        m_startStopButton = new QPushButton("Start", this);
        m_startStopButton->setMinimumHeight(28);

        contentLayout->addStretch(1);

        scrollArea->setWidget(content);
        mainLayout->addWidget(scrollArea, 1);

        // Keep status and Start/Stop visible below the scrolling settings
        auto* footer = new QWidget(this);
        auto* footerLayout = new QVBoxLayout(footer);
        footerLayout->setContentsMargins(6, 6, 6, 6);
        footerLayout->setSpacing(6);
        footerLayout->addWidget(m_planSummaryLabel);
        footerLayout->addWidget(statusGroup);
        footerLayout->addWidget(m_progressBar);
        footerLayout->addWidget(m_failureLabel);
        footerLayout->addWidget(m_startStopButton);
        mainLayout->addWidget(footer);
    }

    // Updates available detector choices
    void RecordingWidget::setAvailableCameras(const QStringList& cameraIds)
    {
        m_availableCameraIds = cameraIds;
        for (int row = 0; row < m_channelTable->rowCount(); ++row)
        {
            // Offer the new cameras, keeping the choice where the camera still exists
            if (auto* combo = qobject_cast<QComboBox*>(m_channelTable->cellWidget(row, 6)))
            {
                const QString selected = combo->currentText();
                combo->clear();
                combo->addItem(QStringLiteral("All"));
                combo->addItems(cameraIds);
                const int index = combo->findText(selected);
                combo->setCurrentIndex(index < 0 ? 0 : index);
            }
        }

        QString current = m_detectorCombo->currentText();
        {
            QSignalBlocker blocker(m_detectorCombo);
            m_detectorCombo->clear();
            m_detectorCombo->addItem("All");
            m_detectorCombo->addItems(cameraIds);
            int idx = m_detectorCombo->findText(current);
            if (idx >= 0)
            {
                m_detectorCombo->setCurrentIndex(idx);
            }
        }

        updateUiState();
    }

    // Opens a save directory picker
    void RecordingWidget::onBrowseClicked()
    {
        const QString startDir = getLastSaveDirectory();
        QString dir = QFileDialog::getExistingDirectory(this, "Select Save Directory", startDir);
        if (dir.isEmpty()) return;
        m_saveDirLineEdit->setText(dir);
    }

    // Generates a timestamp based recording name
    void RecordingWidget::onAutoNameClicked()
    {
        m_fileNameLineEdit->setText(buildTimestampBaseName());
    }

    // Toggles recording between start and stop
    void RecordingWidget::onStartStopClicked()
    {
        if (m_isRecording)
        {
            m_scopeonecore->stopRecording();
        }
        else
        {
            startRecording();
        }
    }

    // Puts a short summary of the settings in the title of each folded block
    void RecordingWidget::updateSectionSummaries()
    {
        if (!m_timeSection)
        {
            return;
        }
        QString time = QStringLiteral("%1 time point(s)").arg(m_framesSpin->value());
        if (m_mdaIntervalSpin->value() > 0.0)
        {
            time += QStringLiteral(", every %1 ms").arg(m_mdaIntervalSpin->value(), 0, 'f', 1);
        }
        if (m_burstModeCheck->isChecked())
        {
            time += QStringLiteral(", %1 bursts").arg(m_burstCountSpin->value());
        }
        m_timeSection->setSummary(time);

        m_zSection->setSummary(m_mdaEnableZCheck->isChecked()
            ? QStringLiteral("%1 slice(s), step %2 µm%3")
                  .arg(m_mdaZCountSpin->value())
                  .arg(m_mdaZStepSpin->value(), 0, 'g', 6)
                  .arg(m_mdaZRelativeCheck->isChecked() ? QStringLiteral(", relative") : QString())
            : QStringLiteral("off"));

        if (!m_mdaEnableXYCheck->isChecked())
        {
            m_positionSection->setSummary(QStringLiteral("off"));
        }
        else
        {
            const int count = m_positionTable->rowCount() > 0
                ? m_positionTable->rowCount()
                : m_mdaXCountSpin->value() * m_mdaYCountSpin->value();
            m_positionSection->setSummary(
                QStringLiteral("%1 position(s)%2")
                    .arg(count)
                    .arg(m_mdaAutofocusCheck->isChecked() ? QStringLiteral(", autofocus") : QString()));
        }

        if (!m_mdaEnableChannelCheck->isChecked())
        {
            m_channelSection->setSummary(QStringLiteral("off"));
        }
        else
        {
            QStringList names;
            for (const auto& channel : usedChannels())
            {
                names.append(channel.preset);
            }
            m_channelSection->setSummary(names.isEmpty() ? QStringLiteral("none selected")
                                                         : names.join(QStringLiteral(", ")));
        }

        QStringList options;
        if (m_keepShutterSlicesCheck->isChecked() || m_keepShutterChannelsCheck->isChecked())
        {
            options.append(QStringLiteral("shutter held"));
        }
        if (m_hardwareSequencingCheck->isChecked())
        {
            options.append(QStringLiteral("hardware sequencing"));
        }
        m_advancedSection->setSummary(options.isEmpty() ? QStringLiteral("default")
                                                        : options.join(QStringLiteral(", ")));
    }

    // Tells what the settings will acquire: how many images and how long the exposures take
    void RecordingWidget::updatePlanSummary()
    {
        if (!m_planSummaryLabel)
        {
            return;
        }
        const qint64 frames = m_framesSpin->value();
        const qint64 bursts = m_burstModeCheck->isChecked() ? m_burstCountSpin->value() : 1;
        const qint64 timePoints = frames * bursts;
        const qint64 zCount = m_mdaEnableZCheck->isChecked() ? m_mdaZCountSpin->value() : 1;
        const qint64 positions = !m_mdaEnableXYCheck->isChecked()
            ? 1
            : m_positionTable->rowCount() > 0
                ? static_cast<qint64>(m_positionTable->rowCount())
                : static_cast<qint64>(m_mdaXCountSpin->value()) * m_mdaYCountSpin->value();
        const qint64 cameras = qMax<qsizetype>(1, selectedCameraIds().size());

        // The camera is asked at most once a second, because the settings change often
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_summaryExposureAtMs > 1000)
        {
            double exposureMs = 0.0;
            m_summaryExposureMs = m_scopeonecore->readExposure(QStringLiteral("All"), exposureMs) && exposureMs > 0.0
                ? exposureMs
                : 0.0;
            m_summaryExposureAtMs = now;
        }
        const double cameraExposureMs = m_summaryExposureMs;

        // Images and exposure time of the channels at one position, over all time points
        qint64 imagesPerPosition = 0;
        double exposurePerTimePointMs = 0.0;
        if (m_mdaEnableChannelCheck->isChecked() && !usedChannels().empty())
        {
            for (const auto& channel : usedChannels())
            {
                const qint64 slices = channel.zStack ? zCount : 1;
                const qint64 imagedTimePoints = (timePoints + channel.skipFrames) / (channel.skipFrames + 1);
                imagesPerPosition += imagedTimePoints * slices;
                exposurePerTimePointMs += static_cast<double>(slices)
                    * (channel.exposureMs > 0.0 ? channel.exposureMs : cameraExposureMs);
            }
        }
        else
        {
            imagesPerPosition = timePoints * zCount;
            exposurePerTimePointMs = static_cast<double>(zCount) * cameraExposureMs;
        }
        const qint64 images = imagesPerPosition * positions * cameras;
        exposurePerTimePointMs *= static_cast<double>(positions);

        // A time point takes the interval, or longer when the exposures do not fit in it
        const double timePointMs = (std::max)(m_mdaIntervalSpin->value(), exposurePerTimePointMs);
        double burstMs = exposurePerTimePointMs
            + static_cast<double>(frames - 1) * timePointMs;
        double totalMs = burstMs;
        if (bursts > 1)
        {
            const double burstIntervalMs = intervalToMs(m_burstIntervalSpin->value(),
                                                        m_burstIntervalUnitCombo->currentText());
            totalMs = static_cast<double>(bursts - 1) * (std::max)(burstIntervalMs, burstMs) + burstMs;
        }

        QString text = QStringLiteral("%L1 image(s)").arg(images);
        if (cameraExposureMs > 0.0 || m_mdaIntervalSpin->value() > 0.0)
        {
            text += QStringLiteral(" · %1 %2")
                        .arg(m_mdaIntervalSpin->value() > 0.0 ? QStringLiteral("about")
                                                              : QStringLiteral("at least"),
                             formatDuration(totalMs));
        }
        m_planSummaryLabel->setText(text);
    }

    // Updates widget enabled state from recording settings
    void RecordingWidget::updateUiState()
    {
        const bool editingEnabled = !m_isRecording && !m_writerFinalizing;
        const bool burstEnabled = m_burstModeCheck->isChecked();
        const bool hasSelectedCameras = !selectedCameraIds().isEmpty();

        m_detectorCombo->setEnabled(editingEnabled);
        m_saveDirLineEdit->setEnabled(editingEnabled);
        m_browseButton->setEnabled(editingEnabled);
        m_fileNameLineEdit->setEnabled(editingEnabled);
        m_autoNameButton->setEnabled(editingEnabled);
        m_formatCombo->setEnabled(editingEnabled);
        const bool binaryFormat =
            m_formatCombo->currentData().toInt() == static_cast<int>(scopeone::core::RecordingFormat::Binary);
        m_compressionCheck->setEnabled(editingEnabled && !binaryFormat);
        m_compressionLevelSpin->setEnabled(editingEnabled && !binaryFormat && m_compressionCheck->isChecked());
        m_pyramidCheck->setEnabled(
            editingEnabled
            && m_formatCombo->currentData().toInt()
                == static_cast<int>(scopeone::core::RecordingFormat::OmeZarr));
        m_framesSpin->setEnabled(editingEnabled);
        m_burstModeCheck->setEnabled(editingEnabled);
        m_burstCountSpin->setEnabled(editingEnabled && burstEnabled);
        m_burstIntervalSpin->setEnabled(editingEnabled && burstEnabled);
        m_burstIntervalUnitCombo->setEnabled(editingEnabled && burstEnabled);

        m_mdaIntervalSpin->setEnabled(editingEnabled);
        m_mdaOrderList->setEnabled(editingEnabled);
        m_mdaEnableZCheck->setEnabled(editingEnabled);
        m_mdaEnableXYCheck->setEnabled(editingEnabled);
        m_mdaZStartSpin->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaZStepSpin->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaZCountSpin->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaXStartSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaXStepSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaXCountSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYStartSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYStepSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYCountSpin->setEnabled(editingEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaZRelativeCheck->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        {
            const bool listEnabled = editingEnabled && m_mdaEnableXYCheck->isChecked();
            m_positionTable->setEnabled(listEnabled);
            m_positionMarkButton->setEnabled(listEnabled);
            m_positionRemoveButton->setEnabled(listEnabled && m_positionTable->rowCount() > 0);
            m_positionClearButton->setEnabled(listEnabled && m_positionTable->rowCount() > 0);
            m_positionFromGridButton->setEnabled(listEnabled);
            m_positionLoadButton->setEnabled(listEnabled);
            m_positionSaveButton->setEnabled(listEnabled && m_positionTable->rowCount() > 0);
        }
        m_mdaAutofocusCheck->setEnabled(editingEnabled);
        m_mdaAutofocusEverySpin->setEnabled(editingEnabled && m_mdaAutofocusCheck->isChecked());
        m_keepShutterSlicesCheck->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        m_hardwareSequencingCheck->setEnabled(editingEnabled && m_mdaEnableZCheck->isChecked());
        m_commentLineEdit->setEnabled(editingEnabled);
        m_keepShutterChannelsCheck->setEnabled(
            editingEnabled && m_mdaEnableChannelCheck->isChecked() && usedChannels().size() > 1);
        m_mdaEnableChannelCheck->setEnabled(editingEnabled);
        const bool channelsEnabled = editingEnabled && m_mdaEnableChannelCheck->isChecked();
        m_channelGroupCombo->setEnabled(channelsEnabled);
        m_channelTable->setEnabled(channelsEnabled);
        m_channelUpButton->setEnabled(channelsEnabled && m_channelTable->currentRow() > 0);
        m_channelDownButton->setEnabled(
            channelsEnabled && m_channelTable->currentRow() >= 0
            && m_channelTable->currentRow() + 1 < m_channelTable->rowCount());
        const int orderRow = m_mdaOrderList->currentRow();
        const int orderCount = m_mdaOrderList->count();
        m_mdaOrderUpButton->setEnabled(editingEnabled && orderRow > 0);
        m_mdaOrderDownButton->setEnabled(editingEnabled && orderRow >= 0 && orderRow < orderCount - 1);

        const bool hasDir = !m_saveDirLineEdit->text().trimmed().isEmpty();
        const bool hasName = !normalizedBaseName().isEmpty();
        const bool canStart = editingEnabled
            && hasSelectedCameras
            && hasDir
            && hasName;
        m_startStopButton->setEnabled(m_isRecording || canStart);
        updateSectionSummaries();
        updatePlanSummary();
        updateStorageStatus();
    }

    // Queries available storage without blocking the UI thread
    void RecordingWidget::updateStorageStatus()
    {
        if (m_storageQueryPending)
        {
            return;
        }

        m_storageQueryPending = true;
        const QString saveDir = m_saveDirLineEdit->text().trimmed();
        auto* watcher = new QFutureWatcher<qint64>(this);
        connect(watcher, &QFutureWatcher<qint64>::finished, this,
                [this, watcher, saveDir]()
                {
                    const qint64 availableBytes = watcher->result();
                    m_storageQueryPending = false;
                    if (saveDir == m_saveDirLineEdit->text().trimmed())
                    {
                        updateStorageStatusText(availableBytes);
                    }
                    else
                    {
                        updateStorageStatus();
                    }
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run([saveDir]()
        {
            if (saveDir.isEmpty())
            {
                return qint64{-1};
            }
            QStorageInfo storage(saveDir);
            storage.refresh();
            return storage.isValid() && storage.isReady()
                       ? storage.bytesAvailable()
                       : qint64{-1};
        }));
    }

    // Updates storage text from one completed background query
    void RecordingWidget::updateStorageStatusText(qint64 availableBytes)
    {
        QStringList details;
        if (availableBytes >= 0)
        {
            details.append(QStringLiteral("%1 free").arg(formatByteCount(availableBytes)));
        }

        const QStringList cameraIds = selectedCameraIds();
        QStringList layerKeys;
        layerKeys.reserve(cameraIds.size());
        for (const QString& cameraId : cameraIds)
        {
            layerKeys.append(scopeone::core::ScopeOneCore::rawLayerKey(cameraId));
        }
        const QList<scopeone::core::ImageFrame> frames = m_scopeonecore->graphFrames(layerKeys);
        qint64 bytesPerPlane = 0;
        int validFrameCount = 0;
        for (const auto& frame : frames)
        {
            if (frame.isValid())
            {
                bytesPerPlane += frame.payloadByteCount();
                ++validFrameCount;
            }
        }
        if (!cameraIds.isEmpty() && validFrameCount == cameraIds.size() && bytesPerPlane > 0)
        {
            const qint64 burstCount = m_burstModeCheck->isChecked() ? m_burstCountSpin->value() : 1;
            const qint64 zCount = m_mdaEnableZCheck->isChecked() ? m_mdaZCountSpin->value() : 1;
            const qint64 positionCount = !m_mdaEnableXYCheck->isChecked()
                ? 1
                : m_positionTable->rowCount() > 0
                    ? static_cast<qint64>(m_positionTable->rowCount())
                    : static_cast<qint64>(m_mdaXCountSpin->value()) * m_mdaYCountSpin->value();
            const qint64 channelCount = m_mdaEnableChannelCheck->isChecked()
                ? static_cast<qint64>((std::max<size_t>)(1, usedChannels().size()))
                : 1;
            const long double estimatedBytes = static_cast<long double>(bytesPerPlane)
                * m_framesSpin->value() * burstCount * zCount * positionCount * channelCount;
            const qint64 estimate = estimatedBytes > static_cast<long double>((std::numeric_limits<qint64>::max)())
                                        ? (std::numeric_limits<qint64>::max)()
                                        : static_cast<qint64>(estimatedBytes);
            details.append(QStringLiteral("%1 estimated raw").arg(formatByteCount(estimate)));
            if (availableBytes >= 0 && estimate > availableBytes)
            {
                details.append(QStringLiteral("insufficient free space"));
            }
        }

        m_storageStatusLabel->setText(details.isEmpty()
                                          ? QStringLiteral("Storage: unavailable")
                                          : QStringLiteral("Storage: %1").arg(details.join(QStringLiteral(", "))));
    }

    // Reads the last save directory from settings
    QString RecordingWidget::getLastSaveDirectory() const
    {
        QSettings settings("ScopeOne", "ScopeOne");
        QString lastDir = settings.value("LastSaveDirectory", QDir::homePath()).toString();
        if (!QDir(lastDir).exists())
        {
            return QDir::homePath();
        }
        return lastDir;
    }

    // Stores the last save directory in settings
    void RecordingWidget::setLastSaveDirectory(const QString& path)
    {
        QSettings settings("ScopeOne", "ScopeOne");
        settings.setValue("LastSaveDirectory", path);
    }

    // Builds a timestamp base name for recording output
    QString RecordingWidget::buildTimestampBaseName() const
    {
        return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    }

    // Returns a filesystem safe base name from the editor
    QString RecordingWidget::normalizedBaseName() const
    {
        return QFileInfo(m_fileNameLineEdit->text().trimmed()).fileName();
    }

    // Resolves the selected detector list
    QStringList RecordingWidget::selectedCameraIds() const
    {
        const QString selected = m_detectorCombo->currentText().trimmed();
        if (selected.compare("All", Qt::CaseInsensitive) == 0)
        {
            return m_availableCameraIds;
        }
        if (selected.isEmpty())
        {
            return {};
        }
        if (m_availableCameraIds.contains(selected))
        {
            return {selected};
        }
        return {};
    }

    // Collects form values and starts recording
    bool RecordingWidget::startRecording()
    {
        QString saveDir = m_saveDirLineEdit->text().trimmed();
        if (saveDir.isEmpty())
        {
            qWarning().noquote() << "Save directory is empty";
            return false;
        }
        QString baseName = normalizedBaseName();
        if (baseName.isEmpty())
        {
            baseName = buildTimestampBaseName();
            m_fileNameLineEdit->setText(baseName);
        }

        scopeone::core::ExperimentPlan plan;
        plan.format = static_cast<scopeone::core::RecordingFormat>(m_formatCombo->currentData().toInt());
        plan.enableCompression =
            plan.format != scopeone::core::RecordingFormat::Binary && m_compressionCheck->isChecked();
        plan.compressionLevel = m_compressionLevelSpin->value();
        plan.enablePyramid = plan.format == scopeone::core::RecordingFormat::OmeZarr
            && m_pyramidCheck->isChecked();
        plan.framesPerBurst = m_framesSpin->value();
        plan.burstMode = m_burstModeCheck->isChecked();
        plan.targetBursts = plan.burstMode ? m_burstCountSpin->value() : 1;
        plan.burstIntervalMs = intervalToMs(m_burstIntervalSpin->value(), m_burstIntervalUnitCombo->currentText());
        plan.mdaIntervalMs = m_mdaIntervalSpin->value();
        plan.saveDir = saveDir;
        plan.baseName = baseName;
        plan.order.clear();
        for (int i = 0; i < m_mdaOrderList->count(); ++i)
        {
            plan.order.push_back(static_cast<scopeone::core::ScopeOneCore::RecordingAxis>(
                m_mdaOrderList->item(i)->data(Qt::UserRole).toInt()));
        }
        std::reverse(plan.order.begin(), plan.order.end());

        if (m_mdaEnableZCheck->isChecked())
        {
            // A relative stack is placed around the focus position at the start of the recording
            double zOrigin = 0.0;
            if (m_mdaZRelativeCheck->isChecked()
                && !m_scopeonecore->readZPosition(m_scopeonecore->currentFocusDevice(), zOrigin))
            {
                qWarning().noquote() << "Cannot read the focus position for a relative Z stack";
                return false;
            }
            const int count = qMax(1, m_mdaZCountSpin->value());
            plan.zPositions.reserve(count);
            for (int i = 0; i < count; ++i)
            {
                plan.zPositions.push_back(zOrigin + m_mdaZStartSpin->value()
                    + static_cast<double>(i) * m_mdaZStepSpin->value());
            }
        }
        plan.keepShutterOpenSlices = m_mdaEnableZCheck->isChecked() && m_keepShutterSlicesCheck->isChecked();
        plan.hardwareSequencing = m_mdaEnableZCheck->isChecked() && m_hardwareSequencingCheck->isChecked();
        plan.comment = m_commentLineEdit->text().trimmed();
        plan.autofocus = m_mdaAutofocusCheck->isChecked();
        plan.autofocusEveryFrames = plan.autofocus ? m_mdaAutofocusEverySpin->value() : 0;

        if (m_mdaEnableXYCheck->isChecked() && m_positionTable->rowCount() > 0)
        {
            for (int row = 0; row < m_positionTable->rowCount(); ++row)
            {
                const auto text = [this, row](int column)
                {
                    const QTableWidgetItem* item = m_positionTable->item(row, column);
                    return item ? item->text().trimmed() : QString();
                };
                bool xOk = false;
                bool yOk = false;
                const double x = text(1).toDouble(&xOk);
                const double y = text(2).toDouble(&yOk);
                if (!xOk || !yOk)
                {
                    qWarning().noquote() << QStringLiteral("Position %1 has no valid X and Y").arg(row + 1);
                    return false;
                }
                bool zOk = false;
                const double z = text(3).toDouble(&zOk);
                if (!text(3).isEmpty() && !zOk)
                {
                    qWarning().noquote() << QStringLiteral("Position %1 has an invalid Z").arg(row + 1);
                    return false;
                }
                plan.positions.push_back(QPointF(x, y));
                plan.positionLabels.push_back(text(0).isEmpty() ? QStringLiteral("Pos%1").arg(row + 1) : text(0));
                plan.positionZ.push_back(zOk ? std::optional<double>(z) : std::nullopt);
            }
        }
        else if (m_mdaEnableXYCheck->isChecked())
        {
            const int xCount = qMax(1, m_mdaXCountSpin->value());
            const int yCount = qMax(1, m_mdaYCountSpin->value());
            plan.positions.reserve(static_cast<size_t>(xCount) * static_cast<size_t>(yCount));
            for (int yIndex = 0; yIndex < yCount; ++yIndex)
            {
                const double y = m_mdaYStartSpin->value()
                    + static_cast<double>(yIndex) * m_mdaYStepSpin->value();
                for (int xIndex = 0; xIndex < xCount; ++xIndex)
                {
                    const double x = m_mdaXStartSpin->value()
                        + static_cast<double>(xIndex) * m_mdaXStepSpin->value();
                    plan.positions.push_back(QPointF(x, y));
                    // Named like the tiles of a Micro-Manager position list
                    plan.positionLabels.push_back(QStringLiteral("Pos-1-%1_%2")
                                                      .arg(xIndex, 3, 10, QLatin1Char('0'))
                                                      .arg(yIndex, 3, 10, QLatin1Char('0')));
                }
            }
        }

        if (m_mdaEnableChannelCheck->isChecked())
        {
            plan.channels = usedChannels();
            plan.channelGroup = plan.channels.empty() ? QString() : m_channelGroupCombo->currentText();
            plan.keepShutterOpenChannels = plan.channels.size() > 1 && m_keepShutterChannelsCheck->isChecked();
        }

        if (!m_scopeonecore->startRecording(plan, selectedCameraIds()))
        {
            qWarning().noquote() << QStringLiteral("Failed to start recording");
            return false;
        }
        saveLastSettings();
        return true;
    }

    // Captures the Acquire target into the gallery
    bool RecordingWidget::snapToGallery(const QString& target)
    {
        const QString normalizedTarget = target.trimmed();
        QStringList cameraIds;
        if (normalizedTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            cameraIds = m_availableCameraIds;
        }
        else if (m_availableCameraIds.contains(normalizedTarget))
        {
            cameraIds = {normalizedTarget};
        }
        if (cameraIds.isEmpty())
        {
            qWarning().noquote() << "No camera available for gallery capture";
            return false;
        }

        scopeone::core::ExperimentPlan capturedPlan;
        capturedPlan.streamToDisk = false;
        capturedPlan.format = static_cast<scopeone::core::RecordingFormat>(m_formatCombo->currentData().toInt());
        capturedPlan.enableCompression =
            capturedPlan.format != scopeone::core::RecordingFormat::Binary && m_compressionCheck->isChecked();
        capturedPlan.compressionLevel = m_compressionLevelSpin->value();
        capturedPlan.enablePyramid = capturedPlan.format == scopeone::core::RecordingFormat::OmeZarr
            && m_pyramidCheck->isChecked();
        capturedPlan.saveDir = m_saveDirLineEdit->text().trimmed();
        const QString captureBase = normalizedBaseName().isEmpty() ? buildTimestampBaseName() : normalizedBaseName();
        capturedPlan.baseName = captureBase + QStringLiteral("_capture_")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));

        QStringList layerKeys;
        layerKeys.reserve(cameraIds.size());
        for (const QString& cameraId : cameraIds)
        {
            layerKeys.append(scopeone::core::ScopeOneCore::rawLayerKey(cameraId));
        }
        const QList<scopeone::core::ImageFrame> frames = m_scopeonecore->graphFrames(layerKeys);
        auto capturedSession = m_scopeonecore->createFrameSession(
            frames,
            capturedPlan);
        if (!capturedSession)
        {
            qWarning().noquote() << "No current frame available to append to gallery";
            return false;
        }

        emit gallerySessionCaptured(capturedSession);
        qInfo().noquote() << QStringLiteral("Gallery appended with %1 frame(s)").arg(frames.size());
        return true;
    }

    // Adds one row to the position list
    void RecordingWidget::addListedPosition(const QString& label, double x, double y, const QString& z)
    {
        const int row = m_positionTable->rowCount();
        m_positionTable->insertRow(row);
        m_positionTable->setItem(row, 0, new QTableWidgetItem(label));
        m_positionTable->setItem(row, 1, new QTableWidgetItem(QString::number(x, 'f', 3)));
        m_positionTable->setItem(row, 2, new QTableWidgetItem(QString::number(y, 'f', 3)));
        m_positionTable->setItem(row, 3, new QTableWidgetItem(z));
    }

    // Replaces the list with the positions of the XY grid
    void RecordingWidget::fillPositionListFromGrid()
    {
        m_positionTable->setRowCount(0);
        for (int yIndex = 0; yIndex < qMax(1, m_mdaYCountSpin->value()); ++yIndex)
        {
            for (int xIndex = 0; xIndex < qMax(1, m_mdaXCountSpin->value()); ++xIndex)
            {
                addListedPosition(QStringLiteral("Pos-1-%1_%2")
                                      .arg(xIndex, 3, 10, QLatin1Char('0'))
                                      .arg(yIndex, 3, 10, QLatin1Char('0')),
                                  m_mdaXStartSpin->value() + xIndex * m_mdaXStepSpin->value(),
                                  m_mdaYStartSpin->value() + yIndex * m_mdaYStepSpin->value(),
                                  QString());
            }
        }
        updateUiState();
    }

    // Loads a Micro-Manager position list into the list
    void RecordingWidget::loadPositionList()
    {
        const QString path = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("Load position list"),
                                                          m_saveDirLineEdit->text(),
                                                          QStringLiteral("Position lists (*.pos *.json *.txt);;All files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        QList<scopeone::gui::ListedPosition> positions;
        QString error;
        if (!scopeone::gui::PositionListFile::load(path, positions, &error))
        {
            qWarning().noquote() << QStringLiteral("Failed to load the position list: %1").arg(error);
            return;
        }
        m_positionTable->setRowCount(0);
        for (int index = 0; index < positions.size(); ++index)
        {
            const auto& position = positions.at(index);
            addListedPosition(position.label.isEmpty() ? QStringLiteral("Pos%1").arg(index + 1) : position.label,
                              position.x,
                              position.y,
                              position.z ? QString::number(*position.z, 'f', 3) : QString());
        }
        updateUiState();
    }

    // Saves the list as a Micro-Manager position list
    void RecordingWidget::savePositionList()
    {
        const QString path = QFileDialog::getSaveFileName(this,
                                                          QStringLiteral("Save position list"),
                                                          m_saveDirLineEdit->text() + QStringLiteral("/positions.pos"),
                                                          QStringLiteral("Position lists (*.pos)"));
        if (path.isEmpty())
        {
            return;
        }
        QList<scopeone::gui::ListedPosition> positions;
        for (int row = 0; row < m_positionTable->rowCount(); ++row)
        {
            const auto text = [this, row](int column)
            {
                const QTableWidgetItem* item = m_positionTable->item(row, column);
                return item ? item->text().trimmed() : QString();
            };
            scopeone::gui::ListedPosition position;
            position.label = text(0);
            position.x = text(1).toDouble();
            position.y = text(2).toDouble();
            bool zOk = false;
            const double z = text(3).toDouble(&zOk);
            if (zOk)
            {
                position.z = z;
            }
            positions.push_back(position);
        }
        QString error;
        if (!scopeone::gui::PositionListFile::save(path,
                                                   positions,
                                                   m_scopeonecore->currentXYStageDevice(),
                                                   m_scopeonecore->currentFocusDevice(),
                                                   &error))
        {
            qWarning().noquote() << QStringLiteral("Failed to save the position list: %1").arg(error);
        }
    }

    // Camera choice of one channel: every selected camera, or one of them
    QComboBox* RecordingWidget::makeChannelCameraCombo(const QString& selected)
    {
        auto* combo = new QComboBox(m_channelTable);
        combo->addItem(QStringLiteral("All"));
        combo->addItems(m_availableCameraIds);
        const int index = combo->findText(selected);
        combo->setCurrentIndex(index < 0 ? 0 : index);
        return combo;
    }

    // Shows the colour of a channel on its button
    void RecordingWidget::showChannelColor(QPushButton* button, const QString& color)
    {
        button->setProperty("channelColor", color);
        button->setText(color.isEmpty() ? QStringLiteral("Auto") : color);
        button->setStyleSheet(color.isEmpty()
            ? QString()
            : QStringLiteral("QPushButton { background-color: %1; color: %2; }")
                  .arg(color, QColor(color).lightness() < 128 ? QStringLiteral("white")
                                                               : QStringLiteral("black")));
    }

    // Colour choice of one channel. Auto leaves the colour to the viewer.
    QPushButton* RecordingWidget::makeChannelColorButton(const QString& color)
    {
        auto* button = new QPushButton(m_channelTable);
        showChannelColor(button, color);
        connect(button, &QPushButton::clicked, this, [this, button]()
        {
            const QString current = button->property("channelColor").toString();
            const QColor chosen = QColorDialog::getColor(current.isEmpty() ? QColor(Qt::white) : QColor(current),
                                                         this,
                                                         QStringLiteral("Channel color"));
            if (chosen.isValid())
            {
                showChannelColor(button, chosen.name(QColor::HexRgb));
            }
        });
        auto* reset = new QAction(QStringLiteral("Auto"), button);
        connect(reset, &QAction::triggered, button, [button]() { showChannelColor(button, QString()); });
        button->addAction(reset);
        button->setContextMenuPolicy(Qt::ActionsContextMenu);
        return button;
    }

    // Lists the configuration groups of the loaded hardware as channel groups
    void RecordingWidget::refreshChannelGroups()
    {
        const QStringList groups = m_scopeonecore->availableConfigGroups();
        QStringList shown;
        for (int index = 0; index < m_channelGroupCombo->count(); ++index)
        {
            shown.append(m_channelGroupCombo->itemText(index));
        }
        if (groups == shown)
        {
            return;
        }
        const QString previous = m_channelGroupCombo->currentText().isEmpty()
            ? m_savedChannelGroup
            : m_channelGroupCombo->currentText();
        {
            QSignalBlocker blocker(m_channelGroupCombo);
            m_channelGroupCombo->clear();
            m_channelGroupCombo->addItems(groups);
            // Micro-Manager calls the group of its channels "Channel" by default
            int selected = m_channelGroupCombo->findText(previous);
            if (selected < 0)
            {
                selected = m_channelGroupCombo->findText(QStringLiteral("Channel"));
            }
            m_channelGroupCombo->setCurrentIndex(selected < 0 ? 0 : selected);
        }
        loadChannelPresets();
    }

    // Lists the presets of the selected group, keeping the settings of those already listed
    void RecordingWidget::loadChannelPresets()
    {
        struct Row
        {
            bool used{false};
            scopeone::core::ChannelSetting setting;
        };
        QHash<QString, Row> previous;
        QStringList previousOrder;
        for (int row = 0; row < m_channelTable->rowCount(); ++row)
        {
            Row entry;
            entry.used = m_channelTable->item(row, 0)->checkState() == Qt::Checked;
            entry.setting.preset = m_channelTable->item(row, 1)->text();
            entry.setting.exposureMs = m_channelTable->item(row, 2)->data(Qt::EditRole).toDouble();
            entry.setting.zOffsetUm = m_channelTable->item(row, 3)->data(Qt::EditRole).toDouble();
            entry.setting.zStack = m_channelTable->item(row, 4)->checkState() == Qt::Checked;
            entry.setting.skipFrames = m_channelTable->item(row, 5)->data(Qt::EditRole).toInt();
            if (auto* combo = qobject_cast<QComboBox*>(m_channelTable->cellWidget(row, 6)))
            {
                entry.setting.cameraId = combo->currentText() == QStringLiteral("All")
                    ? QString()
                    : combo->currentText();
            }
            if (const auto* colorButton = qobject_cast<QPushButton*>(m_channelTable->cellWidget(row, 7)))
            {
                entry.setting.color = colorButton->property("channelColor").toString();
            }
            previous.insert(entry.setting.preset, entry);
            previousOrder.append(entry.setting.preset);
        }

        // Channels of a saved protocol or of the last session, before the table has any
        if (previous.isEmpty() && !m_savedChannels.isEmpty()
            && (m_savedChannelGroup.isEmpty() || m_savedChannelGroup == m_channelGroupCombo->currentText()))
        {
            for (const QJsonValue& value : m_savedChannels)
            {
                const QJsonObject saved = value.toObject();
                Row entry;
                entry.used = saved.value(QStringLiteral("used")).toBool();
                entry.setting.preset = saved.value(QStringLiteral("preset")).toString();
                entry.setting.exposureMs = saved.value(QStringLiteral("exposureMs")).toDouble();
                entry.setting.zOffsetUm = saved.value(QStringLiteral("zOffsetUm")).toDouble();
                entry.setting.zStack = saved.value(QStringLiteral("zStack")).toBool(true);
                entry.setting.skipFrames = saved.value(QStringLiteral("skipFrames")).toInt();
                const QString camera = saved.value(QStringLiteral("camera")).toString();
                entry.setting.cameraId = camera == QStringLiteral("All") ? QString() : camera;
                entry.setting.color = saved.value(QStringLiteral("color")).toString();
                if (!entry.setting.preset.isEmpty())
                {
                    previous.insert(entry.setting.preset, entry);
                    previousOrder.append(entry.setting.preset);
                }
            }
        }

        QStringList presets = m_scopeonecore->availableConfigs(m_channelGroupCombo->currentText());
        QStringList ordered;
        for (const QString& preset : previousOrder)
        {
            if (presets.contains(preset))
            {
                ordered.append(preset);
            }
        }
        for (const QString& preset : presets)
        {
            if (!ordered.contains(preset))
            {
                ordered.append(preset);
            }
        }

        {
            QSignalBlocker blocker(m_channelTable);
            m_channelTable->setRowCount(0);
            for (const QString& preset : ordered)
            {
                const Row entry = previous.value(preset);
                const int row = m_channelTable->rowCount();
                m_channelTable->insertRow(row);

                auto* use = new QTableWidgetItem();
                use->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                use->setCheckState(entry.used ? Qt::Checked : Qt::Unchecked);
                auto* name = new QTableWidgetItem(preset);
                name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                auto* exposure = new QTableWidgetItem();
                exposure->setData(Qt::EditRole, entry.setting.exposureMs);
                auto* offset = new QTableWidgetItem();
                offset->setData(Qt::EditRole, entry.setting.zOffsetUm);
                auto* zStack = new QTableWidgetItem();
                zStack->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                zStack->setCheckState(previous.contains(preset) && !entry.setting.zStack ? Qt::Unchecked
                                                                                         : Qt::Checked);
                auto* skip = new QTableWidgetItem();
                skip->setData(Qt::EditRole, entry.setting.skipFrames);

                m_channelTable->setItem(row, 0, use);
                m_channelTable->setItem(row, 1, name);
                m_channelTable->setItem(row, 2, exposure);
                m_channelTable->setItem(row, 3, offset);
                m_channelTable->setItem(row, 4, zStack);
                m_channelTable->setItem(row, 5, skip);
                m_channelTable->setCellWidget(row, 6, makeChannelCameraCombo(entry.setting.cameraId));
                m_channelTable->setCellWidget(row, 7, makeChannelColorButton(entry.setting.color));
            }
        }
        if (m_channelTable->rowCount() > 0)
        {
            m_savedChannels = QJsonArray();
        }
        syncOrderList();
        updateUiState();
    }

    // Swaps the selected channel with its neighbour
    void RecordingWidget::moveChannelRow(int delta)
    {
        const int row = m_channelTable->currentRow();
        const int target = row + delta;
        if (row < 0 || target < 0 || target >= m_channelTable->rowCount())
        {
            return;
        }
        {
            QSignalBlocker blocker(m_channelTable);
            for (int column = 0; column < 6; ++column)
            {
                QTableWidgetItem* first = m_channelTable->takeItem(row, column);
                QTableWidgetItem* second = m_channelTable->takeItem(target, column);
                m_channelTable->setItem(row, column, second);
                m_channelTable->setItem(target, column, first);
            }
            auto* firstColor = qobject_cast<QPushButton*>(m_channelTable->cellWidget(row, 7));
            auto* secondColor = qobject_cast<QPushButton*>(m_channelTable->cellWidget(target, 7));
            if (firstColor && secondColor)
            {
                const QString color = firstColor->property("channelColor").toString();
                showChannelColor(firstColor, secondColor->property("channelColor").toString());
                showChannelColor(secondColor, color);
            }
            auto* firstCamera = qobject_cast<QComboBox*>(m_channelTable->cellWidget(row, 6));
            auto* secondCamera = qobject_cast<QComboBox*>(m_channelTable->cellWidget(target, 6));
            if (firstCamera && secondCamera)
            {
                const QString text = firstCamera->currentText();
                firstCamera->setCurrentText(secondCamera->currentText());
                secondCamera->setCurrentText(text);
            }
        }
        m_channelTable->setCurrentCell(target, 1);
        updateUiState();
    }

    // The checked channels in the order of the table
    std::vector<scopeone::core::ChannelSetting> RecordingWidget::usedChannels() const
    {
        std::vector<scopeone::core::ChannelSetting> channels;
        for (int row = 0; row < m_channelTable->rowCount(); ++row)
        {
            if (m_channelTable->item(row, 0)->checkState() != Qt::Checked)
            {
                continue;
            }
            scopeone::core::ChannelSetting channel;
            channel.preset = m_channelTable->item(row, 1)->text();
            channel.exposureMs = (std::max)(0.0, m_channelTable->item(row, 2)->data(Qt::EditRole).toDouble());
            channel.zOffsetUm = m_channelTable->item(row, 3)->data(Qt::EditRole).toDouble();
            channel.zStack = m_channelTable->item(row, 4)->checkState() == Qt::Checked;
            channel.skipFrames = (std::max)(0, m_channelTable->item(row, 5)->data(Qt::EditRole).toInt());
            if (auto* combo = qobject_cast<QComboBox*>(m_channelTable->cellWidget(row, 6)))
            {
                if (combo->currentText() != QStringLiteral("All"))
                {
                    channel.cameraId = combo->currentText();
                }
            }
            if (const auto* colorButton = qobject_cast<QPushButton*>(m_channelTable->cellWidget(row, 7)))
            {
                channel.color = colorButton->property("channelColor").toString();
            }
            channels.push_back(channel);
        }
        return channels;
    }

    // Moves one MDA order item in the visible list
    void RecordingWidget::moveOrderItem(int delta)
    {
        const int row = m_mdaOrderList->currentRow();
        const int newRow = row + delta;
        if (row < 0 || newRow < 0 || newRow >= m_mdaOrderList->count())
        {
            return;
        }
        QListWidgetItem* item = m_mdaOrderList->takeItem(row);
        m_mdaOrderList->insertItem(newRow, item);
        m_mdaOrderList->setCurrentRow(newRow);
        std::vector<int> next;
        next.reserve(m_orderPreference.size());
        for (int i = 0; i < m_mdaOrderList->count(); ++i)
        {
            next.push_back(m_mdaOrderList->item(i)->data(Qt::UserRole).toInt());
        }
        std::reverse(next.begin(), next.end());
        for (int axis : m_orderPreference)
        {
            if (std::find(next.begin(), next.end(), axis) == next.end())
            {
                next.push_back(axis);
            }
        }
        m_orderPreference = std::move(next);
        updateUiState();
    }

    // Rebuilds the visible MDA order list from preferences
    void RecordingWidget::syncOrderList()
    {
        const int currentAxis = m_mdaOrderList->currentItem()
                                    ? m_mdaOrderList->currentItem()->data(Qt::UserRole).toInt()
                                    : static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time);

        QSet<int> allowed;
        allowed.insert(static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time));
        if (m_mdaEnableZCheck->isChecked())
        {
            allowed.insert(static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Z));
        }
        if (m_mdaEnableXYCheck->isChecked())
        {
            allowed.insert(static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::XY));
        }
        if (m_mdaEnableChannelCheck->isChecked() && !usedChannels().empty())
        {
            allowed.insert(static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Channel));
        }

        std::vector<int> visibleInternal;
        for (int axis : m_orderPreference)
        {
            if (allowed.contains(axis))
            {
                visibleInternal.push_back(axis);
            }
        }
        if (std::find(visibleInternal.begin(), visibleInternal.end(),
                      static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time)) == visibleInternal.end())
        {
            visibleInternal.insert(visibleInternal.begin(),
                                   static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time));
        }

        QSignalBlocker blocker(m_mdaOrderList);
        m_mdaOrderList->clear();
        std::reverse(visibleInternal.begin(), visibleInternal.end());
        for (int axis : visibleInternal)
        {
            QString label;
            switch (static_cast<scopeone::core::ScopeOneCore::RecordingAxis>(axis))
            {
            case scopeone::core::ScopeOneCore::RecordingAxis::Time:
                label = "Time";
                break;
            case scopeone::core::ScopeOneCore::RecordingAxis::Z:
                label = "Z";
                break;
            case scopeone::core::ScopeOneCore::RecordingAxis::XY:
                label = "XY";
                break;
            case scopeone::core::ScopeOneCore::RecordingAxis::Channel:
                label = "Channel";
                break;
            }
            auto* item = new QListWidgetItem(label, m_mdaOrderList);
            item->setData(Qt::UserRole, axis);
        }

        int selectRow = 0;
        for (int i = 0; i < m_mdaOrderList->count(); ++i)
        {
            auto* item = m_mdaOrderList->item(i);
            if (item->data(Qt::UserRole).toInt() == currentAxis)
            {
                selectRow = i;
                break;
            }
        }
        m_mdaOrderList->setCurrentRow(selectRow);
    }

} // namespace scopeone::ui
