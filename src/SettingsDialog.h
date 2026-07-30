#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace scopeone::ui
{
    class SettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit SettingsDialog(qint64 maxPendingWriteBytes,
                                const QString& microManagerDirectory,
                                QWidget* parent = nullptr);

        qint64 maxPendingWriteBytes() const;
        QString microManagerDirectory() const;

    private:
        QLineEdit* m_recordingBufferLimitEdit{nullptr};
        QLineEdit* m_microManagerDirectoryEdit{nullptr};
    };
}
