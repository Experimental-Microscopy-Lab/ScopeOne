#include "RecordingWidget.h"
#include "scopeone/ScopeOneCore.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
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

    QString recordingResultMessage(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
    {
        return session ? session->saveMessage() : QString();
    }

    bool recordingResultSuccess(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
    {
        return session && session->isSaved();
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

    QString formatStatusText(int phase,
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
        QString status = phaseText(phase);
        if (phase == scopeone::core::kRecordingPhaseWaitingNextBurst && waitRemainingMs > 0)
        {
            status += QString(" (%1 ms)").arg(waitRemainingMs);
        }

        if (phase != scopeone::core::kRecordingPhaseRecordingMda || mdaTimeIndex <= 0)
        {
            return status;
        }

        QString axis = QString("T %1/%2").arg(mdaTimeIndex).arg((std::max)(1, mdaTimeCount));
        if (mdaZCount > 1 && mdaZIndex > 0)
        {
            axis += QString(" Z %1/%2").arg(mdaZIndex).arg(mdaZCount);
        }
        if (mdaPositionCount > 1 && mdaPositionIndex > 0)
        {
            axis += QString(" XY %1/%2").arg(mdaPositionIndex).arg(mdaPositionCount);
        }

        QString pos;
        if (hasXY)
        {
            pos = QString("X=%1 Y=%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3);
        }
        if (hasZ)
        {
            const QString zText = QString("Current Z=%1").arg(z, 0, 'f', 3);
            pos = pos.isEmpty() ? zText : (pos + " " + zText);
        }

        if (!axis.isEmpty() && !pos.isEmpty())
        {
            return QString("%1 [%2 | %3]").arg(status, axis, pos);
        }
        if (!axis.isEmpty())
        {
            return QString("%1 [%2]").arg(status, axis);
        }
        return status;
    }

    QString formatFramesText(qint64 frameCurrent, qint64 frameTarget)
    {
        const qint64 target = (std::max)(0ll, frameTarget);
        return QString("Frames: %1 / %2").arg(frameCurrent).arg(target);
    }

    QString formatBurstsText(int burstCurrent, int burstTarget)
    {
        if (burstTarget <= 0)
        {
            return QStringLiteral("Bursts: 0");
        }
        return QString("Bursts: %1/%2").arg(burstCurrent).arg(burstTarget);
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
            return QStringLiteral("Stopping");
        case RecordingWriterPhase::Completed:
            return QStringLiteral("Completed");
        case RecordingWriterPhase::Failed:
            return QStringLiteral("Failed");
        }
        return QStringLiteral("Idle");
    }

    QString formatWriterStatusText(const scopeone::core::ScopeOneCore::RecordingWriterStatus& status)
    {
        QString text = QStringLiteral("Writer: %1").arg(writerPhaseText(status.phase()));
        if (status.phase() == scopeone::core::ScopeOneCore::RecordingWriterPhase::Failed && !status.errorMessage().
            isEmpty())
        {
            return QStringLiteral("%1 - %2").arg(text, status.errorMessage());
        }

        QStringList details;
        if (status.framesWritten() > 0 || status.phase() ==
            scopeone::core::ScopeOneCore::RecordingWriterPhase::Completed)
        {
            details.append(QStringLiteral("%1 frame(s)").arg(status.framesWritten()));
        }
        if (status.maxPendingWriteBytes() > 0)
        {
            details.append(QStringLiteral("%1 / %2 queued")
                           .arg(formatByteCount(status.pendingWriteBytes()))
                           .arg(formatByteCount(status.maxPendingWriteBytes())));
        }
        else if (status.pendingWriteBytes() > 0)
        {
            details.append(QStringLiteral("%1 queued").arg(formatByteCount(status.pendingWriteBytes())));
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
        connect(m_burstModeCheck, &QCheckBox::toggled, this, &RecordingWidget::onBurstModeToggled);
        connect(m_detectorCombo, &QComboBox::currentTextChanged, this, &RecordingWidget::onDetectorChanged);
        connect(m_snapToGalleryButton, &QPushButton::clicked, this, &RecordingWidget::onSnapToGalleryClicked);
        connect(m_saveDirLineEdit, &QLineEdit::textChanged, this, [this]() { updateUiState(); });
        connect(m_fileNameLineEdit, &QLineEdit::textChanged, this, [this]() { updateUiState(); });
        connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this]() { updateUiState(); });
        connect(m_compressionCheck, &QCheckBox::toggled, this, [this]() { updateUiState(); });
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
                    m_statusLabel->setText(formatStatusText(phase,
                                                            waitRemainingMs,
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
                                                            z));
                    m_frameCountLabel->setText(formatFramesText(frameCurrent, frameTarget));
                    m_burstCountLabel->setText(formatBurstsText(burstCurrent, burstTarget));
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStateChanged, this,
                [this](bool recording)
                {
                    m_isRecording = recording;
                    m_startStopButton->setText(recording ? "Stop" : "Start");
                    updateUiState();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingWriterStatusChanged, this,
                [this](const scopeone::core::ScopeOneCore::RecordingWriterStatus& status)
                {
                    m_writerStatusLabel->setText(formatWriterStatusText(status));
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::recordingStopped, this,
                [this](const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
                {
                    const QString result = recordingResultMessage(session);
                    const bool saved = recordingResultSuccess(session);
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
                    const QString result = recordingResultMessage(session);
                    const bool saved = recordingResultSuccess(session);
                    if (saved)
                    {
                        m_writerStatusLabel->setText(QStringLiteral("Writer: Completed"));
                    }
                    else if (!result.isEmpty())
                    {
                        m_writerStatusLabel->setText(QStringLiteral("Writer: Failed - %1").arg(result));
                    }
                    else
                    {
                        m_writerStatusLabel->setText(QStringLiteral("Writer: Failed - Error: no session data"));
                    }
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

        updateUiState();
    }

    RecordingWidget::~RecordingWidget()
    {
        stopRecording();
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

        auto* galleryRow = new QHBoxLayout();
        galleryRow->setSpacing(6);
        m_snapToGalleryButton = new QPushButton("Snap to Gallery", this);
        galleryRow->addWidget(m_snapToGalleryButton);
        captureLayout->addWidget(new QLabel("Gallery:", this), 3, 0);
        captureLayout->addLayout(galleryRow, 3, 1, 1, 2);

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

        contentLayout->addWidget(formatGroup);

        auto* mdaGroup = new QGroupBox("MDA (Time / Z / XY)", this);
        auto* mdaLayout = new QFormLayout(mdaGroup);

        m_framesSpin = new QSpinBox(this);
        m_framesSpin->setRange(1, 1000000);
        m_framesSpin->setValue(100);
        mdaLayout->addRow("Frames:", m_framesSpin);

        m_mdaIntervalSpin = new QDoubleSpinBox(this);
        m_mdaIntervalSpin->setRange(0.0, 1000000.0);
        m_mdaIntervalSpin->setDecimals(1);
        m_mdaIntervalSpin->setValue(0.0);
        mdaLayout->addRow("Interval (ms):", m_mdaIntervalSpin);

        m_mdaEnableZCheck = new QCheckBox("Enable Z Stack", this);
        mdaLayout->addRow("", m_mdaEnableZCheck);

        auto* zRowLayout = new QHBoxLayout();
        zRowLayout->setSpacing(6);
        m_mdaZStartSpin = new QDoubleSpinBox(this);
        m_mdaZStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaZStartSpin->setDecimals(3);
        m_mdaZStartSpin->setValue(0.0);
        m_mdaZStepSpin = new QDoubleSpinBox(this);
        m_mdaZStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaZStepSpin->setDecimals(3);
        m_mdaZStepSpin->setValue(1.0);
        m_mdaZCountSpin = new QSpinBox(this);
        m_mdaZCountSpin->setRange(1, 10000);
        m_mdaZCountSpin->setValue(1);
        zRowLayout->addWidget(new QLabel("Start", this));
        zRowLayout->addWidget(m_mdaZStartSpin);
        zRowLayout->addWidget(new QLabel("Step", this));
        zRowLayout->addWidget(m_mdaZStepSpin);
        zRowLayout->addWidget(new QLabel("Count", this));
        zRowLayout->addWidget(m_mdaZCountSpin);
        zRowLayout->addStretch();
        mdaLayout->addRow("Z:", zRowLayout);

        m_mdaEnableXYCheck = new QCheckBox("Enable XY Grid", this);
        mdaLayout->addRow("", m_mdaEnableXYCheck);

        auto* xRowLayout = new QHBoxLayout();
        xRowLayout->setSpacing(6);
        m_mdaXStartSpin = new QDoubleSpinBox(this);
        m_mdaXStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaXStartSpin->setDecimals(3);
        m_mdaXStartSpin->setValue(0.0);
        m_mdaXStepSpin = new QDoubleSpinBox(this);
        m_mdaXStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaXStepSpin->setDecimals(3);
        m_mdaXStepSpin->setValue(1.0);
        m_mdaXCountSpin = new QSpinBox(this);
        m_mdaXCountSpin->setRange(1, 10000);
        m_mdaXCountSpin->setValue(1);
        xRowLayout->addWidget(new QLabel("Start", this));
        xRowLayout->addWidget(m_mdaXStartSpin);
        xRowLayout->addWidget(new QLabel("Step", this));
        xRowLayout->addWidget(m_mdaXStepSpin);
        xRowLayout->addWidget(new QLabel("Count", this));
        xRowLayout->addWidget(m_mdaXCountSpin);
        xRowLayout->addStretch();
        mdaLayout->addRow("X:", xRowLayout);

        auto* yRowLayout = new QHBoxLayout();
        yRowLayout->setSpacing(6);
        m_mdaYStartSpin = new QDoubleSpinBox(this);
        m_mdaYStartSpin->setRange(-1000000.0, 1000000.0);
        m_mdaYStartSpin->setDecimals(3);
        m_mdaYStartSpin->setValue(0.0);
        m_mdaYStepSpin = new QDoubleSpinBox(this);
        m_mdaYStepSpin->setRange(-1000000.0, 1000000.0);
        m_mdaYStepSpin->setDecimals(3);
        m_mdaYStepSpin->setValue(1.0);
        m_mdaYCountSpin = new QSpinBox(this);
        m_mdaYCountSpin->setRange(1, 10000);
        m_mdaYCountSpin->setValue(1);
        yRowLayout->addWidget(new QLabel("Start", this));
        yRowLayout->addWidget(m_mdaYStartSpin);
        yRowLayout->addWidget(new QLabel("Step", this));
        yRowLayout->addWidget(m_mdaYStepSpin);
        yRowLayout->addWidget(new QLabel("Count", this));
        yRowLayout->addWidget(m_mdaYCountSpin);
        yRowLayout->addStretch();
        mdaLayout->addRow("Y:", yRowLayout);

        m_mdaOrderList = new QListWidget(this);
        m_mdaOrderList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_orderPreference = {
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time),
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Z),
            static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::XY)
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

        contentLayout->addWidget(mdaGroup);

        auto* statusGroup = new QGroupBox("Status", this);
        auto* statusLayout = new QVBoxLayout(statusGroup);
        m_statusLabel = new QLabel("Idle", this);
        m_writerStatusLabel = new QLabel("Writer: Idle", this);
        m_frameCountLabel = new QLabel("Frames: 0", this);
        m_burstCountLabel = new QLabel("Bursts: 0", this);
        statusLayout->addWidget(m_statusLabel);
        statusLayout->addWidget(m_writerStatusLabel);
        statusLayout->addWidget(m_frameCountLabel);
        statusLayout->addWidget(m_burstCountLabel);
        contentLayout->addWidget(statusGroup);

        m_startStopButton = new QPushButton("Start", this);
        m_startStopButton->setMinimumHeight(28);
        contentLayout->addWidget(m_startStopButton);

        contentLayout->addStretch(1);

        scrollArea->setWidget(content);
        mainLayout->addWidget(scrollArea);
    }

    // Updates available detector choices
    void RecordingWidget::setAvailableCameras(const QStringList& cameraIds)
    {
        m_availableCameraIds = cameraIds;

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
        setLastSaveDirectory(dir);
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
            stopRecording();
        }
        else
        {
            startRecording();
        }
    }

    // Refreshes UI state when burst mode changes
    void RecordingWidget::onBurstModeToggled(bool)
    {
        updateUiState();
    }

    // Refreshes UI state when detector selection changes
    void RecordingWidget::onDetectorChanged(const QString&)
    {
        updateUiState();
    }

    // Appends current frames to the gallery
    void RecordingWidget::onSnapToGalleryClicked()
    {
        appendSelectedFramesToGallery();
    }

    // Updates widget enabled state from recording settings
    void RecordingWidget::updateUiState()
    {
        const bool editingEnabled = !m_isRecording;
        const bool burstEnabled = m_burstModeCheck->isChecked();
        const bool hasSelectedCameras = !selectedCameraIds().isEmpty();

        m_detectorCombo->setEnabled(editingEnabled);
        m_saveDirLineEdit->setEnabled(editingEnabled);
        m_browseButton->setEnabled(editingEnabled);
        m_fileNameLineEdit->setEnabled(editingEnabled);
        m_autoNameButton->setEnabled(editingEnabled);
        m_snapToGalleryButton->setEnabled(hasSelectedCameras);
        m_formatCombo->setEnabled(editingEnabled);
        const bool binaryFormat =
            m_formatCombo->currentData().toInt() == static_cast<int>(scopeone::core::RecordingFormat::Binary);
        m_compressionCheck->setEnabled(editingEnabled && !binaryFormat);
        m_compressionLevelSpin->setEnabled(editingEnabled && !binaryFormat && m_compressionCheck->isChecked());
        m_framesSpin->setEnabled(editingEnabled);
        m_burstModeCheck->setEnabled(editingEnabled);
        m_burstCountSpin->setEnabled(editingEnabled && burstEnabled);
        m_burstIntervalSpin->setEnabled(editingEnabled && burstEnabled);
        m_burstIntervalUnitCombo->setEnabled(editingEnabled && burstEnabled);

        const bool mdaEnabled = editingEnabled;
        m_mdaIntervalSpin->setEnabled(mdaEnabled);
        m_mdaOrderList->setEnabled(mdaEnabled);
        m_mdaEnableZCheck->setEnabled(mdaEnabled);
        m_mdaEnableXYCheck->setEnabled(mdaEnabled);
        m_mdaZStartSpin->setEnabled(mdaEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaZStepSpin->setEnabled(mdaEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaZCountSpin->setEnabled(mdaEnabled && m_mdaEnableZCheck->isChecked());
        m_mdaXStartSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaXStepSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaXCountSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYStartSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYStepSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        m_mdaYCountSpin->setEnabled(mdaEnabled && m_mdaEnableXYCheck->isChecked());
        const int orderRow = m_mdaOrderList->currentRow();
        const int orderCount = m_mdaOrderList->count();
        m_mdaOrderUpButton->setEnabled(mdaEnabled && orderRow > 0);
        m_mdaOrderDownButton->setEnabled(mdaEnabled && orderRow >= 0 && orderRow < orderCount - 1);

        const bool hasCameras = hasSelectedCameras;
        const bool hasDir = !m_saveDirLineEdit->text().trimmed().isEmpty();
        const bool hasName = !normalizedBaseName().isEmpty();
        const bool canStart = !m_isRecording && hasCameras && hasDir && hasName;
        m_startStopButton->setEnabled(m_isRecording || canStart);
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
            auto* item = m_mdaOrderList->item(i);
            switch (item->data(Qt::UserRole).toInt())
            {
            case static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time):
                plan.order.push_back(scopeone::core::ScopeOneCore::RecordingAxis::Time);
                break;
            case static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Z):
                plan.order.push_back(scopeone::core::ScopeOneCore::RecordingAxis::Z);
                break;
            case static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::XY):
                plan.order.push_back(scopeone::core::ScopeOneCore::RecordingAxis::XY);
                break;
            default:
                break;
            }
        }
        std::reverse(plan.order.begin(), plan.order.end());
        if (plan.order.empty())
        {
            plan.order = {
                scopeone::core::ScopeOneCore::RecordingAxis::Time,
                scopeone::core::ScopeOneCore::RecordingAxis::Z,
                scopeone::core::ScopeOneCore::RecordingAxis::XY
            };
        }

        if (m_mdaEnableZCheck->isChecked())
        {
            const int count = qMax(1, m_mdaZCountSpin->value());
            plan.zPositions.reserve(count);
            for (int i = 0; i < count; ++i)
            {
                plan.zPositions.push_back(m_mdaZStartSpin->value()
                    + static_cast<double>(i) * m_mdaZStepSpin->value());
            }
        }

        if (m_mdaEnableXYCheck->isChecked())
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
                }
            }
        }

        if (!m_scopeonecore->startRecording(plan, selectedCameraIds()))
        {
            qWarning().noquote() << QStringLiteral("Failed to start recording");
            return false;
        }
        return true;
    }

    // Captures latest selected frames into the gallery
    bool RecordingWidget::appendSelectedFramesToGallery()
    {
        const QStringList cameraIds = selectedCameraIds();
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
        if (!m_orderPreference.empty())
        {
            std::vector<int> visibleDisplay;
            visibleDisplay.reserve(m_mdaOrderList->count());
            for (int i = 0; i < m_mdaOrderList->count(); ++i)
            {
                auto* it = m_mdaOrderList->item(i);
                visibleDisplay.push_back(it->data(Qt::UserRole).toInt());
            }
            std::vector<int> internalVisible;
            internalVisible.reserve(visibleDisplay.size());
            for (int i = static_cast<int>(visibleDisplay.size()) - 1; i >= 0; --i)
            {
                internalVisible.push_back(visibleDisplay[static_cast<size_t>(i)]);
            }
            std::vector<int> next = internalVisible;
            for (int axis : m_orderPreference)
            {
                if (std::find(internalVisible.begin(), internalVisible.end(), axis) == internalVisible.end())
                {
                    next.push_back(axis);
                }
            }
            m_orderPreference = next;
        }
        updateUiState();
    }

    // Rebuilds the visible MDA order list from preferences
    void RecordingWidget::syncOrderList()
    {
        if (m_orderPreference.empty())
        {
            m_orderPreference = {
                static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Time),
                static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::Z),
                static_cast<int>(scopeone::core::ScopeOneCore::RecordingAxis::XY)
            };
        }

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
        std::vector<int> visibleDisplay;
        visibleDisplay.reserve(visibleInternal.size());
        for (int i = static_cast<int>(visibleInternal.size()) - 1; i >= 0; --i)
        {
            visibleDisplay.push_back(visibleInternal[static_cast<size_t>(i)]);
        }
        for (int axis : visibleDisplay)
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

    // Stops the active recording through core
    void RecordingWidget::stopRecording()
    {
        m_scopeonecore->stopRecording();
    }
} // namespace scopeone::ui
