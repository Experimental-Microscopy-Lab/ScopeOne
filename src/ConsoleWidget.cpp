#include "ConsoleWidget.h"
#include <QMetaObject>
#include <QScrollBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QAction>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QShortcut>
#include <QTextEdit>
#include <QTextStream>
#include <QtCore/qlogging.h>
#include <atomic>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        static std::atomic<ConsoleWidget*> g_consoleSink{nullptr};
        static QtMessageHandler g_previousMessageHandler = nullptr;
        static bool g_messageHandlerInstalled = false;
        static QMutex g_pendingMessagesMutex;

        struct PendingConsoleMessage
        {
            QString message;
            QString type;
            QDateTime timestamp;
        };

        static QList<PendingConsoleMessage> g_pendingMessages;

        // Forward Qt messages into the active console widget
        static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
        {
            QString t = "INFO";
            switch (type)
            {
            case QtDebugMsg: t = "DEBUG";
                break;
            case QtInfoMsg: t = "INFO";
                break;
            case QtWarningMsg: t = "WARNING";
                break;
            case QtCriticalMsg: t = "ERROR";
                break;
            case QtFatalMsg: t = "ERROR";
                break;
            }

            QObject* app = QCoreApplication::instance();
            if (g_consoleSink.load() && app)
            {
                QMetaObject::invokeMethod(app, [m=msg, T=t, ts=QDateTime::currentDateTime()]()
                {
                    if (ConsoleWidget* sink = g_consoleSink.load())
                    {
                        sink->addMessage(m, T, ts);
                    }
                }, Qt::QueuedConnection);
            }
            else
            {
                QMutexLocker locker(&g_pendingMessagesMutex);
                constexpr int kMaxPendingMessages = 1000;
                if (g_pendingMessages.size() >= kMaxPendingMessages)
                {
                    g_pendingMessages.removeFirst();
                }
                g_pendingMessages.append({msg, t, QDateTime::currentDateTime()});
            }
            if (g_previousMessageHandler && g_previousMessageHandler != qtMessageHandler)
            {
                g_previousMessageHandler(type, context, msg);
            }
        }

        void installQtMessageHandlerOnce()
        {
            if (g_messageHandlerInstalled)
            {
                return;
            }
            g_previousMessageHandler = qInstallMessageHandler(qtMessageHandler);
            g_messageHandlerInstalled = true;
        }
    }

    // Create the live log console and its filters
    ConsoleWidget::ConsoleWidget(QWidget* parent)
        : QWidget(parent)
          , m_showTimestamps(true)
          , m_autoScroll(true)
    {
        setupUI();
        connect(m_clearButton, &QPushButton::clicked, this, &ConsoleWidget::onClearClicked);
        connect(m_showTimestampsCheckBox, &QCheckBox::toggled, this, &ConsoleWidget::onShowTimestampsToggled);
        connect(m_autoScrollCheckBox, &QCheckBox::toggled, this, &ConsoleWidget::onAutoScrollToggled);
        connect(m_filterComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ConsoleWidget::onFilterChanged);
        connect(m_searchInput, &QLineEdit::textChanged, this,
                [this](const QString& text)
                {
                    m_searchKeyword = text.trimmed();
                    updateDisplay();
                });
        connect(m_runButton, &QPushButton::clicked, this,
                [this]() { executeCommand(m_commandInput->text()); });
        m_consoleTextEdit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_consoleTextEdit, &QTextEdit::customContextMenuRequested,
                this, &ConsoleWidget::showContextMenu);
        m_commandInput->installEventFilter(this);

        auto* clearShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
        clearShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(clearShortcut, &QShortcut::activated, this, &ConsoleWidget::clearMessages);

        updateDisplay();
    }

    // Remove this widget as the Qt message sink when destroyed
    ConsoleWidget::~ConsoleWidget()
    {
        if (g_consoleSink.load() == this)
        {
            g_consoleSink.store(nullptr);
        }
    }

    // Build the console text view and interactive command controls
    void ConsoleWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(6, 6, 6, 6);
        mainLayout->setSpacing(6);

        m_consoleTextEdit = new QTextEdit(this);
        m_consoleTextEdit->setReadOnly(true);
#ifdef Q_OS_WIN
        QFont consoleFont(QStringLiteral("Consolas"), 9);
#else
        QFont consoleFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        consoleFont.setPointSize(9);
#endif
        m_consoleTextEdit->setFont(consoleFont);
        m_consoleTextEdit->setStyleSheet(
            "QTextEdit {"
            "    background-color: #1e1e1e;"
            "    border: 1px solid #343a40;"
            "    selection-background-color: #364fc7;"
            "}"
        );

        auto* topBarLayout = new QVBoxLayout();
        topBarLayout->setSpacing(4);
        topBarLayout->setContentsMargins(0, 0, 0, 0);

        auto* row1Layout = new QHBoxLayout();
        row1Layout->setSpacing(4);

        m_searchInput = new QLineEdit(this);
        m_searchInput->setPlaceholderText(tr("Search logs..."));
        m_searchInput->setClearButtonEnabled(true);
        m_searchInput->setMinimumWidth(80);

        m_filterComboBox = new QComboBox(this);
        m_filterComboBox->addItems({tr("All"),
                                    QStringLiteral("INFO"),
                                    QStringLiteral("DEBUG"),
                                    QStringLiteral("WARNING"),
                                    QStringLiteral("ERROR")});
        m_filterComboBox->setFixedWidth(80);

        m_clearButton = new QPushButton(tr("Clear"), this);
        m_clearButton->setFixedWidth(50);

        row1Layout->addWidget(m_searchInput, 1);
        row1Layout->addWidget(m_filterComboBox);
        row1Layout->addWidget(m_clearButton);

        auto* row2Layout = new QHBoxLayout();
        row2Layout->setSpacing(8);

        m_showTimestampsCheckBox = new QCheckBox(tr("Timestamps"), this);
        m_showTimestampsCheckBox->setChecked(m_showTimestamps);

        m_autoScrollCheckBox = new QCheckBox(tr("Auto-scroll"), this);
        m_autoScrollCheckBox->setChecked(m_autoScroll);

        m_messageCountLabel = new QLabel(tr("Messages: 0"), this);
        m_messageCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        row2Layout->addWidget(m_showTimestampsCheckBox);
        row2Layout->addWidget(m_autoScrollCheckBox);
        row2Layout->addStretch(1);
        row2Layout->addWidget(m_messageCountLabel);

        topBarLayout->addLayout(row1Layout);
        topBarLayout->addLayout(row2Layout);

        mainLayout->addLayout(topBarLayout, 0);
        mainLayout->addWidget(m_consoleTextEdit, 1);

        auto* commandLayout = new QHBoxLayout();
        auto* promptLabel = new QLabel(QStringLiteral(">>> "), this);
        promptLabel->setStyleSheet(QStringLiteral("color: #51cf66;"));
        promptLabel->setFont(consoleFont);

        m_commandInput = new QLineEdit(this);
        m_commandInput->setPlaceholderText(tr("Enter a command, then press Enter"));
        m_commandInput->setFont(consoleFont);
        m_commandInput->setStyleSheet(
            QStringLiteral("QLineEdit { background: #1e1e1e; color: #f8f9fa; "
                           "border: 1px solid #495057; padding: 3px 6px; }"));

        m_runButton = new QPushButton(tr("Run"), this);
        m_runButton->setFixedWidth(64);

        commandLayout->addWidget(promptLabel);
        commandLayout->addWidget(m_commandInput, 1);
        commandLayout->addWidget(m_runButton);
        mainLayout->addLayout(commandLayout, 0);
    }

    // Append one message and trim old history
    void ConsoleWidget::addMessage(const QString& message, const QString& type, const QDateTime& timestamp)
    {
        ConsoleMessage msg;
        msg.message = message;
        msg.type = type.toUpper();
        msg.timestamp = timestamp;

        constexpr int kMaxMessages = 10000;
        if (m_messages.size() >= kMaxMessages)
        {
            m_messages.removeFirst();
        }

        m_messages.append(msg);

        if (messageMatchesCurrentFilter(msg))
        {
            QString formattedMessage = formatMessage(msg);
            m_consoleTextEdit->append(formattedMessage);

            if (m_autoScroll)
            {
                scrollToBottom();
            }
        }
        if (m_messageFilter.isEmpty() && m_searchKeyword.isEmpty())
        {
            m_messageCountLabel->setText(tr("Messages: %1").arg(m_messages.size()));
        }
        else
        {
            updateDisplay();
        }
    }

    // Clear all stored and visible messages
    void ConsoleWidget::clearMessages()
    {
        m_messages.clear();
        m_consoleTextEdit->clear();
        m_messageCountLabel->setText(tr("Messages: 0"));
    }

    // Toggle timestamp rendering for stored log entries
    void ConsoleWidget::setShowTimestamps(bool show)
    {
        if (m_showTimestamps != show)
        {
            m_showTimestamps = show;
            m_showTimestampsCheckBox->setChecked(show);
            updateDisplay();
        }
    }

    bool ConsoleWidget::isShowTimestamps() const
    {
        return m_showTimestamps;
    }

    // Toggle automatic scrolling to the newest message
    void ConsoleWidget::setAutoScroll(bool autoScroll)
    {
        if (m_autoScroll != autoScroll)
        {
            m_autoScroll = autoScroll;
            m_autoScrollCheckBox->setChecked(autoScroll);
        }
    }

    bool ConsoleWidget::isAutoScroll() const
    {
        return m_autoScroll;
    }

    // Replace the active message type filter
    void ConsoleWidget::setMessageFilter(const QStringList& types)
    {
        m_messageFilter = types;
        updateDisplay();
    }

    QStringList ConsoleWidget::getMessageFilter() const
    {
        return m_messageFilter;
    }

    // Rebuild the visible log from filtered history
    void ConsoleWidget::updateDisplay()
    {
        m_consoleTextEdit->setUpdatesEnabled(false);
        m_consoleTextEdit->clear();

        QString htmlContent;
        htmlContent.reserve(m_messages.size() * 100);

        int visibleCount = 0;
        for (const auto& msg : m_messages)
        {
            if (messageMatchesCurrentFilter(msg))
            {
                ++visibleCount;
                htmlContent += formatMessage(msg);
                htmlContent += "<br>";
            }
        }

        m_messageCountLabel->setText(
            visibleCount == m_messages.size()
                ? tr("Messages: %1").arg(m_messages.size())
                : tr("Messages: %1/%2").arg(visibleCount).arg(m_messages.size()));

        if (!htmlContent.isEmpty())
        {
            m_consoleTextEdit->setHtml(htmlContent);
        }

        m_consoleTextEdit->setUpdatesEnabled(true);

        if (m_autoScroll)
        {
            scrollToBottom();
        }
    }

    // Convert one message into colored HTML
    QString ConsoleWidget::formatMessage(const ConsoleMessage& msg) const
    {
        QString timestamp;
        if (m_showTimestamps)
        {
            timestamp = QString("[%1] ").arg(msg.timestamp.toString("hh:mm:ss"));
        }

        QString typePrefix = QString("[%1] ").arg(msg.type);
        QString color = getTypeColor(msg.type);
        QString escapedMessage = msg.message.toHtmlEscaped();
        escapedMessage.replace(QLatin1Char('\n'), QStringLiteral("<br>"));

        return QString("<span style='color: %1'>%2%3%4</span>")
               .arg(color)
               .arg(timestamp)
               .arg(typePrefix)
               .arg(escapedMessage);
    }

    // Pick a display color for a message type
    QString ConsoleWidget::getTypeColor(const QString& type) const
    {
        if (type == "ERROR") return "#ff6b6b";
        if (type == "WARNING") return "#f59f00";
        if (type == "DEBUG") return "#1581ed";
        if (type == "COMMAND") return "#51cf66";
        if (type == "API") return "#66d9ef";
        return "#dee2e6";
    }

    bool ConsoleWidget::messageMatchesCurrentFilter(const ConsoleMessage& msg) const
    {
        if (!m_messageFilter.isEmpty() && !m_messageFilter.contains(msg.type))
        {
            return false;
        }
        if (m_searchKeyword.isEmpty())
        {
            return true;
        }
        return msg.message.contains(m_searchKeyword, Qt::CaseInsensitive)
            || msg.type.contains(m_searchKeyword, Qt::CaseInsensitive);
    }

    // Scroll the console view to the newest message
    void ConsoleWidget::scrollToBottom()
    {
        QScrollBar* scrollBar = m_consoleTextEdit->verticalScrollBar();
        scrollBar->setValue(scrollBar->maximum());
    }

    void ConsoleWidget::onClearClicked()
    {
        clearMessages();
    }

    void ConsoleWidget::onShowTimestampsToggled(bool show)
    {
        setShowTimestamps(show);
    }

    void ConsoleWidget::onAutoScrollToggled(bool autoScroll)
    {
        setAutoScroll(autoScroll);
    }

    // Apply the selected message type filter
    void ConsoleWidget::onFilterChanged()
    {
        QString selectedFilter = m_filterComboBox->currentText();

        if (selectedFilter == tr("All"))
        {
            m_messageFilter.clear();
        }
        else
        {
            m_messageFilter.clear();
            m_messageFilter.append(selectedFilter);
        }

        updateDisplay();
    }

    void ConsoleWidget::setApiDispatcher(ApiDispatcher dispatcher)
    {
        m_apiDispatcher = std::move(dispatcher);
    }

    // Execute one shorthand command or raw Local API request
    void ConsoleWidget::executeCommand(const QString& commandText)
    {
        const QString command = commandText.trimmed();
        if (command.isEmpty())
        {
            return;
        }

        m_commandHistory.append(command);
        if (m_commandHistory.size() > 100)
        {
            m_commandHistory.removeFirst();
        }
        m_historyIndex = m_commandHistory.size();
        m_commandInput->clear();
        addMessage(QStringLiteral(">>> ") + command, QStringLiteral("COMMAND"));

        const QStringList tokens = command.split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString verb = tokens.at(0).toLower();

        if (verb == QStringLiteral("clear") || verb == QStringLiteral("cls"))
        {
            clearMessages();
            return;
        }
        if (verb == QStringLiteral("help") || verb == QStringLiteral("?"))
        {
            showHelp();
            return;
        }

        QJsonObject request;
        if (verb == QStringLiteral("snap"))
        {
            request.insert(QStringLiteral("type"), QStringLiteral("record"));
            request.insert(QStringLiteral("frames"), 1);
            if (tokens.size() > 1)
            {
                request.insert(QStringLiteral("camera"), tokens.at(1));
            }
        }
        else if (verb == QStringLiteral("preview") || verb == QStringLiteral("live"))
        {
            const QString action = tokens.value(1).toLower();
            if (action != QStringLiteral("start")
                && action != QStringLiteral("on")
                && action != QStringLiteral("stop")
                && action != QStringLiteral("off"))
            {
                addMessage(tr("Usage: preview start|stop [camera]"), QStringLiteral("ERROR"));
                return;
            }
            request.insert(QStringLiteral("type"),
                           action == QStringLiteral("stop") || action == QStringLiteral("off")
                               ? QStringLiteral("stop_preview")
                               : QStringLiteral("start_preview"));
            if (tokens.size() > 2)
            {
                request.insert(QStringLiteral("camera"), tokens.at(2));
            }
        }
        else if (verb == QStringLiteral("exp") || verb == QStringLiteral("exposure"))
        {
            bool ok = false;
            const double exposureMs = tokens.value(1).toDouble(&ok);
            if (!ok || exposureMs <= 0.0)
            {
                addMessage(tr("Usage: exp <milliseconds> [camera]"), QStringLiteral("ERROR"));
                return;
            }
            request.insert(QStringLiteral("type"), QStringLiteral("set_exposure"));
            request.insert(QStringLiteral("exposureMs"), exposureMs);
            if (tokens.size() > 2)
            {
                request.insert(QStringLiteral("camera"), tokens.at(2));
            }
        }
        else if (verb == QStringLiteral("stage"))
        {
            const QString axis = tokens.value(1).toLower();
            if (axis == QStringLiteral("z"))
            {
                bool ok = false;
                const double dz = tokens.value(2).toDouble(&ok);
                if (!ok)
                {
                    addMessage(tr("Usage: stage z <dz_um>"), QStringLiteral("ERROR"));
                    return;
                }
                request.insert(QStringLiteral("type"), QStringLiteral("move_z_relative"));
                request.insert(QStringLiteral("dz"), dz);
            }
            else if (axis == QStringLiteral("xy"))
            {
                bool okX = false;
                bool okY = false;
                const double dx = tokens.value(2).toDouble(&okX);
                const double dy = tokens.value(3).toDouble(&okY);
                if (!okX || !okY)
                {
                    addMessage(tr("Usage: stage xy <dx_um> <dy_um>"), QStringLiteral("ERROR"));
                    return;
                }
                request.insert(QStringLiteral("type"), QStringLiteral("move_xy_relative"));
                request.insert(QStringLiteral("dx"), dx);
                request.insert(QStringLiteral("dy"), dy);
            }
            else
            {
                addMessage(tr("Usage: stage z <dz_um> or stage xy <dx_um> <dy_um>"),
                            QStringLiteral("ERROR"));
                return;
            }
        }
        else if (verb == QStringLiteral("roi"))
        {
            const QString action = tokens.value(1).toLower();
            if (action == QStringLiteral("draw"))
            {
                request.insert(QStringLiteral("type"), QStringLiteral("draw_roi"));
            }
            else if (action == QStringLiteral("half"))
            {
                request.insert(QStringLiteral("type"), QStringLiteral("set_half_roi"));
            }
            else if (action == QStringLiteral("clear"))
            {
                request.insert(QStringLiteral("type"), QStringLiteral("clear_roi"));
            }
            else
            {
                addMessage(tr("Usage: roi draw|half|clear [camera]"), QStringLiteral("ERROR"));
                return;
            }
            if (tokens.size() > 2)
            {
                request.insert(QStringLiteral("camera"), tokens.at(2));
            }
        }
        else if (verb == QStringLiteral("fit"))
        {
            request.insert(QStringLiteral("type"), QStringLiteral("set_fit_to_window"));
            request.insert(QStringLiteral("enabled"), true);
        }
        else if (verb == QStringLiteral("zoom"))
        {
            bool ok = false;
            const int zoomPercent = tokens.value(1).toInt(&ok);
            if (!ok || zoomPercent <= 0)
            {
                addMessage(tr("Usage: zoom <percent>"), QStringLiteral("ERROR"));
                return;
            }
            request.insert(QStringLiteral("type"), QStringLiteral("set_zoom"));
            request.insert(QStringLiteral("zoomPercent"), zoomPercent);
        }
        else if (verb == QStringLiteral("auto"))
        {
            request.insert(QStringLiteral("type"), QStringLiteral("auto_layer_levels"));
        }
        else if (verb == QStringLiteral("status"))
        {
            request.insert(QStringLiteral("type"), QStringLiteral("status"));
        }
        else if (verb == QStringLiteral("api"))
        {
            const QString operationType = tokens.value(1);
            if (operationType.isEmpty())
            {
                addMessage(tr("Usage: api <operation_type> [json_payload]"),
                           QStringLiteral("ERROR"));
                return;
            }
            const int payloadStart = command.indexOf(operationType) + operationType.size();
            const QString payload = command.mid(payloadStart).trimmed();
            if (!payload.isEmpty())
            {
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(
                    payload.toUtf8(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject())
                {
                    addMessage(tr("Invalid JSON: %1").arg(parseError.errorString()),
                               QStringLiteral("ERROR"));
                    return;
                }
                request = document.object();
            }
            request.insert(QStringLiteral("type"), operationType);
        }
        else
        {
            addMessage(tr("Unknown command '%1'. Type 'help' for available commands.")
                           .arg(verb),
                       QStringLiteral("WARNING"));
            return;
        }

        m_apiDispatcher(request,
                        [this](const QJsonObject& response)
                        {
                            showCommandResponse(response);
                        });
    }

    // Print the command grammar in the console
    void ConsoleWidget::showHelp()
    {
        addMessage(QStringLiteral(
                        "Commands:\n"
                        "  help, ?                         Show this help\n"
                        "  clear, cls                      Clear the console\n"
                        "  snap [camera]                   Capture one frame\n"
                        "  preview start|stop [camera]     Control live preview\n"
                        "  exp <ms> [camera]               Set exposure\n"
                        "  stage z <dz_um>                 Move focus stage\n"
                        "  stage xy <dx_um> <dy_um>        Move XY stage\n"
                        "  roi draw|half|clear [camera]    Control ROI\n"
                        "  fit                             Fit preview to window\n"
                        "  zoom <percent>                  Set preview zoom\n"
                        "  auto                            Auto-stretch active layer\n"
                        "  status                          Show application status\n"
                        "  api <type> [json]               Send a raw Local API request"),
                    QStringLiteral("INFO"));
    }

    // Show one Local API response in a readable form
    void ConsoleWidget::showCommandResponse(const QJsonObject& response)
    {
        if (!response.value(QStringLiteral("ok")).toBool())
        {
            addMessage(response.value(QStringLiteral("error"))
                           .toString(),
                       QStringLiteral("ERROR"));
            return;
        }

        const QString type = response.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("record"))
        {
            addMessage(tr("Snapshot captured"), QStringLiteral("INFO"));
            return;
        }
        if (type == QStringLiteral("set_exposure")
            && response.contains(QStringLiteral("exposureMs")))
        {
            addMessage(tr("Exposure set to %1 ms")
                           .arg(response.value(QStringLiteral("exposureMs")).toDouble()),
                       QStringLiteral("INFO"));
            return;
        }
        if (type == QStringLiteral("start_preview"))
        {
            addMessage(tr("Live preview started"), QStringLiteral("INFO"));
            return;
        }
        if (type == QStringLiteral("stop_preview"))
        {
            addMessage(tr("Live preview stopped"), QStringLiteral("INFO"));
            return;
        }

        addMessage(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Indented)),
                   QStringLiteral("API"));
    }

    // Export the current visible console text
    void ConsoleWidget::exportLogsToFile()
    {
        const QString filePath = QFileDialog::getSaveFileName(
            this, tr("Export Console Logs"), QString(), tr("Log files (*.log);;All files (*.*)"));
        if (filePath.isEmpty())
        {
            return;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            addMessage(tr("Failed to export logs to %1").arg(filePath), QStringLiteral("ERROR"));
            return;
        }
        QTextStream stream(&file);
        stream << m_consoleTextEdit->toPlainText();
        addMessage(tr("Logs exported to %1").arg(filePath), QStringLiteral("INFO"));
    }

    // Build the console context menu for copy, export, and clear actions
    void ConsoleWidget::showContextMenu(const QPoint& position)
    {
        QMenu menu(this);
        QAction* copySelection = menu.addAction(tr("Copy Selection"));
        copySelection->setEnabled(m_consoleTextEdit->textCursor().hasSelection());
        connect(copySelection, &QAction::triggered,
                m_consoleTextEdit, &QTextEdit::copy);

        QAction* copyAll = menu.addAction(tr("Copy All"));
        connect(copyAll, &QAction::triggered, this,
                [this]()
                {
                    QGuiApplication::clipboard()->setText(m_consoleTextEdit->toPlainText());
                });

        menu.addSeparator();
        QAction* exportAction = menu.addAction(tr("Export Logs to File..."));
        connect(exportAction, &QAction::triggered, this, &ConsoleWidget::exportLogsToFile);

        menu.addSeparator();
        QAction* clearAction = menu.addAction(tr("Clear Console (Ctrl+L)"));
        connect(clearAction, &QAction::triggered, this, &ConsoleWidget::clearMessages);

        menu.exec(m_consoleTextEdit->viewport()->mapToGlobal(position));
    }

    // Provide terminal-style history navigation in the command input
    bool ConsoleWidget::eventFilter(QObject* object, QEvent* event)
    {
        if (object == m_commandInput && event->type() == QEvent::KeyPress)
        {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Up)
            {
                if (!m_commandHistory.isEmpty())
                {
                    m_historyIndex = qMax(0, m_historyIndex - 1);
                    m_commandInput->setText(m_commandHistory.at(m_historyIndex));
                }
                return true;
            }
            if (keyEvent->key() == Qt::Key_Down)
            {
                if (!m_commandHistory.isEmpty())
                {
                    m_historyIndex = qMin(m_commandHistory.size(), m_historyIndex + 1);
                    m_commandInput->setText(
                        m_historyIndex == m_commandHistory.size()
                            ? QString()
                            : m_commandHistory.at(m_historyIndex));
                }
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            {
                executeCommand(m_commandInput->text());
                return true;
            }
        }
        return QWidget::eventFilter(object, event);
    }

    // Forward Qt log output into this widget
    void ConsoleWidget::installAsQtMessageSink(ConsoleWidget* sink)
    {
        installQtMessageHandlerOnce();
        g_consoleSink.store(sink);

        QList<PendingConsoleMessage> pendingMessages;
        {
            QMutexLocker locker(&g_pendingMessagesMutex);
            pendingMessages.swap(g_pendingMessages);
        }

        for (const PendingConsoleMessage& message : pendingMessages)
        {
            sink->addMessage(message.message, message.type, message.timestamp);
        }
    }

    // Install the Qt log handler before the console widget exists
    void ConsoleWidget::installQtMessageHandler()
    {
        installQtMessageHandlerOnce();
    }
} // namespace scopeone::ui
