#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class QTextEdit;
class QEvent;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLabel;

namespace scopeone::ui
{
    class ConsoleWidget : public QWidget
    {
        Q_OBJECT

    public:
        using ApiDispatcher = std::function<void(
            const QJsonObject&, std::function<void(const QJsonObject&)>)>;

        explicit ConsoleWidget(QWidget* parent = nullptr);
        ~ConsoleWidget() override;

        void addMessage(const QString& message,
                        const QString& type = "INFO",
                        const QDateTime& timestamp = QDateTime::currentDateTime());

        void clearMessages();

        static void installAsQtMessageSink(ConsoleWidget* sink);
        static void installQtMessageHandler();

        void setShowTimestamps(bool show);

        bool isShowTimestamps() const;

        void setAutoScroll(bool autoScroll);

        bool isAutoScroll() const;

        void setMessageFilter(const QStringList& types);

        QStringList getMessageFilter() const;

        void setApiDispatcher(ApiDispatcher dispatcher);

    private:
        void onClearClicked();
        void onShowTimestampsToggled(bool show);
        void onAutoScrollToggled(bool autoScroll);
        void onFilterChanged();

        struct ConsoleMessage
        {
            QString message;
            QString type;
            QDateTime timestamp;
        };

        void setupUI();
        void updateDisplay();
        QString formatMessage(const ConsoleMessage& msg) const;
        QString getTypeColor(const QString& type) const;
        void scrollToBottom();
        void executeCommand(const QString& commandText);
        void showHelp();
        void exportLogsToFile();
        void showContextMenu(const QPoint& position);
        void showCommandResponse(const QJsonObject& response);
        bool eventFilter(QObject* object, QEvent* event) override;

        QTextEdit* m_consoleTextEdit{nullptr};

        QPushButton* m_clearButton{nullptr};
        QPushButton* m_runButton{nullptr};
        QCheckBox* m_showTimestampsCheckBox{nullptr};
        QCheckBox* m_autoScrollCheckBox{nullptr};
        QComboBox* m_filterComboBox{nullptr};
        QLabel* m_messageCountLabel{nullptr};
        QLineEdit* m_searchInput{nullptr};
        QLineEdit* m_commandInput{nullptr};

        bool m_showTimestamps{true};
        bool m_autoScroll{true};
        QStringList m_messageFilter;
        QList<ConsoleMessage> m_messages;
        QString m_searchKeyword;
        QStringList m_commandHistory;
        int m_historyIndex{-1};
        ApiDispatcher m_apiDispatcher;
    };
}
