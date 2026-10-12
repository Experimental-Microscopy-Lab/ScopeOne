#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QComboBox;

namespace scopeone::ui
{
    class SettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit SettingsDialog(qint64 maxPendingWriteBytes,
                                 const QString& microManagerDirectory,
                                 const QString& widgetStyle,
                                 const QString& colorScheme,
                                 QWidget* parent = nullptr);

        qint64 maxPendingWriteBytes() const;
        QString microManagerDirectory() const;
        QString widgetStyle() const;
        QString colorScheme() const;

    private:
        QLineEdit* m_recordingBufferLimitEdit{nullptr};
        QLineEdit* m_microManagerDirectoryEdit{nullptr};
        QComboBox* m_widgetStyleComboBox{nullptr};
        QComboBox* m_colorSchemeComboBox{nullptr};
    };
}
