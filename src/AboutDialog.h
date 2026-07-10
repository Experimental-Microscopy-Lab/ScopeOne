#pragma once

#include <QDialog>

class QTextBrowser;

namespace scopeone::ui
{
    class AboutDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit AboutDialog(QWidget* parent = nullptr);
        ~AboutDialog() override = default;

        static int showAbout(QWidget* parent = nullptr);

    private:
        void setupUI();
        void setContent();

        QTextBrowser* m_contentBrowser{nullptr};
    };
}
