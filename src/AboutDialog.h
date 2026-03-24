#pragma once

#include <QDialog>
#include "AppVersion.h"

class QLabel;

namespace scopeone::ui {

class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override = default;

    static int showAbout(QWidget* parent = nullptr,
                        const QString& appName = SCOPEONE_APP_NAME,
                        const QString& version = SCOPEONE_APP_VERSION_STRING);

private:
    void onOkClicked();

    void setupUI();
    void setContent(const QString& appName, const QString& version);

    QLabel* m_contentLabel{nullptr};
};

}

