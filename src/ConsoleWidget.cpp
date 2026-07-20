#include "ConsoleWidget.h"
#include <QMetaObject>
#include <QScrollBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFont>
#include <QLabel>
#include <QMutex>
#include <QMutexLocker>
#include <QTextEdit>
#include <QtCore/qlogging.h>
#include <atomic>

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

    // Build the console text view and filter controls
    void ConsoleWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(6, 6, 6, 6);
        mainLayout->setSpacing(6);

        m_consoleTextEdit = new QTextEdit(this);
        m_consoleTextEdit->setReadOnly(true);
        m_consoleTextEdit->setFont(QFont("Consolas", 9));
        m_consoleTextEdit->setStyleSheet(
            "QTextEdit {"
            "    background-color: #1e1e1e;"
            "    border: 1px solid #343a40;"
            "    selection-background-color: #364fc7;"
            "}"
        );

        auto* controlLayout = new QHBoxLayout();

        m_clearButton = new QPushButton("Clear", this);
        m_clearButton->setMaximumWidth(60);

        m_showTimestampsCheckBox = new QCheckBox("Timestamps", this);
        m_showTimestampsCheckBox->setChecked(m_showTimestamps);

        m_autoScrollCheckBox = new QCheckBox("Auto-scroll", this);
        m_autoScrollCheckBox->setChecked(m_autoScroll);

        auto* filterLabel = new QLabel("Filter:", this);
        m_filterComboBox = new QComboBox(this);
        m_filterComboBox->addItem("All");
        m_filterComboBox->addItem("INFO");
        m_filterComboBox->addItem("DEBUG");
        m_filterComboBox->addItem("SUCCESS");
        m_filterComboBox->addItem("WARNING");
        m_filterComboBox->addItem("ERROR");
        m_filterComboBox->addItem("SYSTEM");
        m_filterComboBox->setMaximumWidth(120);

        m_messageCountLabel = new QLabel("Messages: 0", this);

        controlLayout->addWidget(m_clearButton);
        controlLayout->addWidget(m_showTimestampsCheckBox);
        controlLayout->addWidget(m_autoScrollCheckBox);
        controlLayout->addWidget(filterLabel);
        controlLayout->addWidget(m_filterComboBox);
        controlLayout->addStretch();
        controlLayout->addWidget(m_messageCountLabel);

        mainLayout->addWidget(m_consoleTextEdit, 1);
        mainLayout->addLayout(controlLayout, 0);
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

        m_messageCountLabel->setText(QString("Messages: %1").arg(m_messages.size()));

        if (m_messageFilter.isEmpty() || m_messageFilter.contains(msg.type))
        {
            QString formattedMessage = formatMessage(msg);
            m_consoleTextEdit->append(formattedMessage);

            if (m_autoScroll)
            {
                scrollToBottom();
            }
        }
    }

    // Clear all stored and visible messages
    void ConsoleWidget::clearMessages()
    {
        m_messages.clear();
        m_consoleTextEdit->clear();
        m_messageCountLabel->setText("Messages: 0");
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

        for (const auto& msg : m_messages)
        {
            if (m_messageFilter.isEmpty() || m_messageFilter.contains(msg.type))
            {
                htmlContent += formatMessage(msg);
                htmlContent += "<br>";
            }
        }

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

        QString typePrefix = QString("[%1] ").arg(msg.type.leftJustified(7));
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
        if (type == "SUCCESS") return "#51cf66";
        if (type == "WARNING") return "#f59f00";
        if (type == "SYSTEM") return "#74c0fc";
        if (type == "DEBUG") return "#adb5bd";
        return "#dee2e6";
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

        if (selectedFilter == "All")
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
