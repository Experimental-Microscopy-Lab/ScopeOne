#include "ParameterDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>

namespace scopeone::ui
{
    bool askParameters(QWidget* parent, const QString& title, QList<ParameterField>& fields)
    {
        QDialog dialog(parent);
        dialog.setWindowTitle(title);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout();
        layout->addLayout(form);

        QList<QWidget*> editors;
        for (const ParameterField& field : fields)
        {
            QWidget* editor = nullptr;
            switch (field.kind)
            {
            case ParameterField::Kind::Integer:
            {
                auto* spin = new QSpinBox(&dialog);
                spin->setRange(static_cast<int>(field.minimum), static_cast<int>(field.maximum));
                spin->setValue(static_cast<int>(field.value));
                spin->setSuffix(field.suffix);
                editor = spin;
                break;
            }
            case ParameterField::Kind::Real:
            {
                auto* spin = new QDoubleSpinBox(&dialog);
                spin->setDecimals(field.decimals);
                spin->setRange(field.minimum, field.maximum);
                spin->setValue(field.value);
                spin->setSuffix(field.suffix);
                editor = spin;
                break;
            }
            case ParameterField::Kind::Choice:
            {
                auto* combo = new QComboBox(&dialog);
                combo->addItems(field.choices);
                combo->setCurrentIndex(field.choice);
                editor = combo;
                break;
            }
            case ParameterField::Kind::Check:
            {
                auto* check = new QCheckBox(&dialog);
                check->setChecked(field.checked);
                editor = check;
                break;
            }
            }
            form->addRow(field.label, editor);
            editors.append(editor);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);

        if (dialog.exec() != QDialog::Accepted)
        {
            return false;
        }
        for (qsizetype index = 0; index < fields.size(); ++index)
        {
            ParameterField& field = fields[index];
            switch (field.kind)
            {
            case ParameterField::Kind::Integer:
                field.value = static_cast<QSpinBox*>(editors.at(index))->value();
                break;
            case ParameterField::Kind::Real:
                field.value = static_cast<QDoubleSpinBox*>(editors.at(index))->value();
                break;
            case ParameterField::Kind::Choice:
                field.choice = static_cast<QComboBox*>(editors.at(index))->currentIndex();
                break;
            case ParameterField::Kind::Check:
                field.checked = static_cast<QCheckBox*>(editors.at(index))->isChecked();
                break;
            }
        }
        return true;
    }
}
