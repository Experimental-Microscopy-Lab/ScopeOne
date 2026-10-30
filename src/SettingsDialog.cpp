#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QStyle>
#include <QStyleFactory>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
    constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;
}

namespace scopeone::ui
{
    // Create the application settings dialog
    SettingsDialog::SettingsDialog(qint64 maxPendingWriteBytes,
                                   const QString& microManagerDirectory,
                                   const QString& widgetStyle,
                                   const QString& colorScheme,
                                   const QString& pythonInterpreter,
                                   QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Settings"));
        setModal(true);

        auto* layout = new QVBoxLayout(this);

        auto* formLayout = new QFormLayout();

        m_widgetStyleComboBox = new QComboBox(this);
        m_widgetStyleComboBox->addItem(QStringLiteral("Default"), QStringLiteral("default"));
        for (const QString& styleKey : QStyleFactory::keys())
        {
            m_widgetStyleComboBox->addItem(styleKey, styleKey);
        }
        const int styleIndex = m_widgetStyleComboBox->findData(widgetStyle);
        m_widgetStyleComboBox->setCurrentIndex(styleIndex >= 0 ? styleIndex : 0);
        formLayout->addRow(QStringLiteral("Widget style"), m_widgetStyleComboBox);

        m_colorSchemeComboBox = new QComboBox(this);
        m_colorSchemeComboBox->addItem(QStringLiteral("System"), QStringLiteral("system"));
        m_colorSchemeComboBox->addItem(QStringLiteral("Light"), QStringLiteral("light"));
        m_colorSchemeComboBox->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
        const int colorSchemeIndex = m_colorSchemeComboBox->findData(colorScheme);
        m_colorSchemeComboBox->setCurrentIndex(colorSchemeIndex >= 0 ? colorSchemeIndex : 0);
        formLayout->addRow(QStringLiteral("Color scheme"), m_colorSchemeComboBox);

        m_recordingBufferLimitEdit = new QLineEdit(this);
        auto* validator = new QDoubleValidator(m_recordingBufferLimitEdit);
        validator->setNotation(QDoubleValidator::StandardNotation);
        m_recordingBufferLimitEdit->setValidator(validator);

        const double valueGiB = (maxPendingWriteBytes > 0)
                                    ? static_cast<double>(maxPendingWriteBytes) / kBytesPerGiB
                                    : 16.0;
        m_recordingBufferLimitEdit->setText(QString::number(valueGiB));
        auto* bufferLimitRow = new QWidget(this);
        auto* bufferLimitLayout = new QHBoxLayout(bufferLimitRow);
        bufferLimitLayout->setContentsMargins(0, 0, 0, 0);
        bufferLimitLayout->setSpacing(6);
        bufferLimitLayout->addWidget(m_recordingBufferLimitEdit);
        bufferLimitLayout->addWidget(new QLabel(QStringLiteral("GiB"), bufferLimitRow));
        bufferLimitLayout->addStretch();
        formLayout->addRow(QStringLiteral("Recording Buffer Limit"), bufferLimitRow);

        auto* microManagerRow = new QWidget(this);
        auto* microManagerLayout = new QHBoxLayout(microManagerRow);
        microManagerLayout->setContentsMargins(0, 0, 0, 0);
        microManagerLayout->setSpacing(6);
        m_microManagerDirectoryEdit = new QLineEdit(microManagerDirectory, microManagerRow);
        m_microManagerDirectoryEdit->setPlaceholderText(QStringLiteral("Bundled adapters only"));
        microManagerLayout->addWidget(m_microManagerDirectoryEdit, 1);
        auto* browseButton = new QToolButton(microManagerRow);
        browseButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        browseButton->setToolTip(QStringLiteral("Select Micro-Manager directory"));
        browseButton->setAutoRaise(true);
        microManagerLayout->addWidget(browseButton);
        formLayout->addRow(QStringLiteral("Micro-Manager Directory"), microManagerRow);

        connect(browseButton, &QToolButton::clicked,
                this, [this]()
                {
                    const QString directory = QFileDialog::getExistingDirectory(
                        this,
                        QStringLiteral("Select Micro-Manager Directory"),
                        m_microManagerDirectoryEdit->text().trimmed());
                    if (!directory.isEmpty())
                    {
                        m_microManagerDirectoryEdit->setText(QDir::toNativeSeparators(directory));
                    }
                });

        // The interpreter that runs Python processing modules, which needs numpy and scopeone
        auto* pythonRow = new QWidget(this);
        auto* pythonLayout = new QHBoxLayout(pythonRow);
        pythonLayout->setContentsMargins(0, 0, 0, 0);
        pythonLayout->setSpacing(6);
        m_pythonInterpreterEdit = new QLineEdit(pythonInterpreter, pythonRow);
#ifdef Q_OS_WIN
        m_pythonInterpreterEdit->setPlaceholderText(QStringLiteral("python"));
#else
        m_pythonInterpreterEdit->setPlaceholderText(QStringLiteral("python3"));
#endif
        m_pythonInterpreterEdit->setToolTip(
            QStringLiteral("Runs Python processing modules. It needs numpy and the scopeone package. "
                           "SCOPEONE_PYTHON overrides it. Takes effect after restart."));
        pythonLayout->addWidget(m_pythonInterpreterEdit, 1);
        auto* pythonBrowseButton = new QToolButton(pythonRow);
        pythonBrowseButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        pythonBrowseButton->setToolTip(QStringLiteral("Select Python interpreter"));
        pythonBrowseButton->setAutoRaise(true);
        pythonLayout->addWidget(pythonBrowseButton);
        formLayout->addRow(QStringLiteral("Python Interpreter"), pythonRow);

        connect(pythonBrowseButton, &QToolButton::clicked,
                this, [this]()
                {
                    const QString path = QFileDialog::getOpenFileName(
                        this,
                        QStringLiteral("Select Python Interpreter"),
                        m_pythonInterpreterEdit->text().trimmed());
                    if (!path.isEmpty())
                    {
                        m_pythonInterpreterEdit->setText(QDir::toNativeSeparators(path));
                    }
                });
        layout->addLayout(formLayout);

        auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttonBox, &QDialogButtonBox::accepted,
                this, [this]()
                {
                    const QString directoryPath = this->microManagerDirectory();
                    if (!directoryPath.isEmpty())
                    {
                        QDir directory(directoryPath);
#ifdef Q_OS_WIN
                        const QStringList adapterFilters{QStringLiteral("mmgr_dal_*.dll")};
#else
                        const QStringList adapterFilters{QStringLiteral("libmmgr_dal_*")};
#endif
                        if (!directory.exists()
                            || directory.entryList(adapterFilters, QDir::Files).isEmpty())
                        {
                            QMessageBox::warning(
                                this,
                                QStringLiteral("Invalid Micro-Manager Directory"),
                                QStringLiteral("Select a directory containing Micro-Manager device adapters."));
                            return;
                        }
                    }
                    accept();
                });
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttonBox);
    }

    // Convert the form value back to bytes
    qint64 SettingsDialog::maxPendingWriteBytes() const
    {
        return static_cast<qint64>(m_recordingBufferLimitEdit->text().toDouble() * kBytesPerGiB);
    }

    // Return the optional external adapter directory
    QString SettingsDialog::microManagerDirectory() const
    {
        const QString directory = m_microManagerDirectoryEdit->text().trimmed();
        return directory.isEmpty() ? QString() : QDir::cleanPath(directory);
    }

    // Return the selected application widget style
    QString SettingsDialog::widgetStyle() const
    {
        return m_widgetStyleComboBox->currentData().toString();
    }

    // Return the interpreter for Python processing modules, empty for the default one
    QString SettingsDialog::pythonInterpreter() const
    {
        return m_pythonInterpreterEdit->text().trimmed();
    }

    // Return the selected application color scheme
    QString SettingsDialog::colorScheme() const
    {
        return m_colorSchemeComboBox->currentData().toString();
    }
} // namespace scopeone::ui
