#include "SettingsDialog.h"

#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace {

constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

}

namespace scopeone::ui {

SettingsDialog::SettingsDialog(qint64 maxPendingWriteBytes, QWidget* parent)
    : QDialog(parent)
{
    // Edit the recording buffer limit in GiB
    setWindowTitle(QStringLiteral("Settings"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    auto* formLayout = new QFormLayout();
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
    layout->addLayout(formLayout);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);
}

qint64 SettingsDialog::maxPendingWriteBytes() const
{
    // Convert the form value back to bytes
    return static_cast<qint64>(m_recordingBufferLimitEdit->text().toDouble() * kBytesPerGiB);
}

} // namespace scopeone::ui
