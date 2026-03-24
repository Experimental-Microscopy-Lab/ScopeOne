#include "ConsoleWidget.h"
#include <QMetaObject>
#include <QScrollBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QTextEdit>

namespace scopeone::ui
{
    namespace
    {
        static ConsoleWidget* g_consoleSink = nullptr;

        static void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
        {
            Q_UNUSED(ctx);
            if (!g_consoleSink) return;
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
            QMetaObject::invokeMethod(g_consoleSink, [m=msg, T=t]()
            {
                if (g_consoleSink) g_consoleSink->addMessage(m, T);
            }, Qt::QueuedConnection);
        }
    }

#include <QDebug>

    ConsoleWidget::ConsoleWidget(QWidget* parent)
        : QWidget(parent)
          , m_showTimestamps(true)
          , m_autoScroll(true)
          , m_messageCount(0)
    {
        setupUI();
        connect(m_clearButton, &QPushButton::clicked, this, &ConsoleWidget::onClearClicked);
        connect(m_showTimestampsCheckBox, &QCheckBox::toggled, this, &ConsoleWidget::onShowTimestampsToggled);
        connect(m_autoScrollCheckBox, &QCheckBox::toggled, this, &ConsoleWidget::onAutoScrollToggled);
        connect(m_filterComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ConsoleWidget::onFilterChanged);

        updateDisplay();
        addMessage("Console initialized", "SYSTEM");
    }

    ConsoleWidget::~ConsoleWidget()
    {
        if (g_consoleSink == this)
        {
            g_consoleSink = nullptr;
            qInstallMessageHandler(nullptr);
        }
    }

    void ConsoleWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);


        m_consoleTextEdit = new QTextEdit(this);
        m_consoleTextEdit->setReadOnly(true);
        m_consoleTextEdit->setFont(QFont("Consolas", 9));
        m_consoleTextEdit->setStyleSheet(
            "QTextEdit {"
            "    background-color: #1e1e1e;"
            "}"
        );

        auto* controlLayout = new QHBoxLayout();

        m_clearButton = new QPushButton("Clear", this);
        m_clearButton->setMaximumWidth(60);

        m_showTimestampsCheckBox = new QCheckBox("Timestamps", this);
        m_showTimestampsCheckBox->setChecked(m_showTimestamps);

        m_autoScrollCheckBox = new QCheckBox("Auto Scroll", this);
        m_autoScrollCheckBox->setChecked(m_autoScroll);

        auto* filterLabel = new QLabel("Filter:", this);
        m_filterComboBox = new QComboBox(this);
        m_filterComboBox->addItem("All Messages");
        m_filterComboBox->addItem("INFO");
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

    void ConsoleWidget::addMessage(const QString& message, const QString& type)
    {
        // Append one message and trim old history
        ConsoleMessage msg;
        msg.message = message;
        msg.type = type.toUpper();
        msg.timestamp = QDateTime::currentDateTime();

        constexpr int kMaxMessages = 10000;
        if (m_messages.size() >= kMaxMessages)
        {
            m_messages.removeFirst();
        }

        m_messages.append(msg);

        m_messageCount++;
        m_messageCountLabel->setText(QString("Messages: %1").arg(m_messageCount));

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

    void ConsoleWidget::clearMessages()
    {
        m_messages.clear();
        m_messageCount = 0;
        m_consoleTextEdit->clear();
        m_messageCountLabel->setText("Messages: 0");
    }

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

    void ConsoleWidget::setMessageFilter(const QStringList& types)
    {
        m_messageFilter = types;
        updateDisplay();
    }

    QStringList ConsoleWidget::getMessageFilter() const
    {
        return m_messageFilter;
    }

    void ConsoleWidget::updateDisplay()
    {
        // Rebuild the visible log from filtered history
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

    QString ConsoleWidget::getTypeColor(const QString& type) const
    {
        if (type == "ERROR") return "#ff0000";
        if (type == "SUCCESS") return "#00ff2a";
        if (type == "WARNING") return "#ffd43b";
        if (type == "SYSTEM") return "#0091ff";
        if (type == "DEBUG") return "#868e96";
        return "#d4d4d4";
    }

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

    void ConsoleWidget::onFilterChanged()
    {
        QString selectedFilter = m_filterComboBox->currentText();

        if (selectedFilter == "All Messages")
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

    void ConsoleWidget::installAsQtMessageSink(ConsoleWidget* sink)
    {
        // Forward Qt log output into this widget
        g_consoleSink = sink;
        qInstallMessageHandler(qtMessageHandler);
    }
} // namespace scopeone::ui
