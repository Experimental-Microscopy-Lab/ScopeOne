#pragma once

#include <QWidget>
#include <QDateTime>

class QTextEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLabel;

namespace scopeone::ui {

class ConsoleWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConsoleWidget(QWidget* parent = nullptr);
    ~ConsoleWidget() override;

    void addMessage(const QString& message, const QString& type = "INFO");

    void clearMessages();

    static void installAsQtMessageSink(ConsoleWidget* sink);

    void setShowTimestamps(bool show);

    bool isShowTimestamps() const;

    void setAutoScroll(bool autoScroll);

    bool isAutoScroll() const;

    void setMessageFilter(const QStringList& types);

    QStringList getMessageFilter() const;

private:
    void onClearClicked();
    void onShowTimestampsToggled(bool show);
    void onAutoScrollToggled(bool autoScroll);
    void onFilterChanged();

    struct ConsoleMessage {
        QString message;
        QString type;
        QDateTime timestamp;
    };

    void setupUI();
    void updateDisplay();
    QString formatMessage(const ConsoleMessage& msg) const;
    QString getTypeColor(const QString& type) const;
    void scrollToBottom();

    QTextEdit* m_consoleTextEdit;

    QPushButton* m_clearButton;
    QCheckBox* m_showTimestampsCheckBox;
    QCheckBox* m_autoScrollCheckBox;
    QComboBox* m_filterComboBox;
    QLabel* m_messageCountLabel;

    bool m_showTimestamps;
    bool m_autoScroll;
    QStringList m_messageFilter;
    int m_messageCount;

    QList<ConsoleMessage> m_messages;
};

}
