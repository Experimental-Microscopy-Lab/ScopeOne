#pragma once

#include <QDialog>

class QLineEdit;

namespace scopeone::ui {

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(qint64 maxPendingWriteBytes, QWidget* parent = nullptr);

    qint64 maxPendingWriteBytes() const;

private:
    QLineEdit* m_recordingBufferLimitEdit{nullptr};
};

}
