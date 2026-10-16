#include "ParameterDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace scopeone::ui
{
    bool askParameters(QWidget* parent, const QString& title, QList<ParameterField>& fields,
                       const PreviewRenderer& preview, const FieldsAction& addToPipeline)
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

        const auto readEditors = [&fields, &editors](QList<ParameterField>& target)
        {
            target = fields;
            for (qsizetype index = 0; index < target.size(); ++index)
            {
                ParameterField& field = target[index];
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
        };

        QLabel* previewLabel = nullptr;
        QTimer* previewTimer = nullptr;
        QCheckBox* previewCheck = nullptr;
        if (preview)
        {
            previewCheck = new QCheckBox(QObject::tr("Preview"), &dialog);
            previewCheck->setChecked(true);
            previewLabel = new QLabel(&dialog);
            previewLabel->setAlignment(Qt::AlignCenter);
            previewLabel->setMinimumHeight(60);
            layout->addWidget(previewCheck);
            layout->addWidget(previewLabel);
            previewTimer = new QTimer(&dialog);
            previewTimer->setSingleShot(true);
            previewTimer->setInterval(120);
            const auto refresh = [&, previewLabel, previewCheck]()
            {
                if (!previewCheck->isChecked())
                {
                    previewLabel->clear();
                    previewLabel->hide();
                    // Takes back the room of the picture once the layout has been updated
                    QTimer::singleShot(0, &dialog, [&dialog]() { dialog.adjustSize(); });
                    return;
                }
                previewLabel->show();
                QList<ParameterField> current;
                readEditors(current);
                previewLabel->setPixmap(QPixmap::fromImage(preview(current)));
                dialog.resize(dialog.size().expandedTo(dialog.sizeHint()));
            };
            QObject::connect(previewTimer, &QTimer::timeout, &dialog, refresh);
            QObject::connect(previewCheck, &QCheckBox::toggled, &dialog, [previewTimer](bool) { previewTimer->start(); });
            for (QWidget* editor : editors)
            {
                if (auto* spin = qobject_cast<QSpinBox*>(editor))
                {
                    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), &dialog, [previewTimer](int) { previewTimer->start(); });
                }
                else if (auto* real = qobject_cast<QDoubleSpinBox*>(editor))
                {
                    QObject::connect(real, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [previewTimer](double) { previewTimer->start(); });
                }
                else if (auto* combo = qobject_cast<QComboBox*>(editor))
                {
                    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [previewTimer](int) { previewTimer->start(); });
                }
                else if (auto* check = qobject_cast<QCheckBox*>(editor))
                {
                    QObject::connect(check, &QCheckBox::toggled, &dialog, [previewTimer](bool) { previewTimer->start(); });
                }
            }
            previewTimer->start();
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (addToPipeline)
        {
            QPushButton* pipelineButton = buttons->addButton(QObject::tr("Add to Pipeline"), QDialogButtonBox::ActionRole);
            QObject::connect(pipelineButton, &QPushButton::clicked, &dialog, [&]()
            {
                QList<ParameterField> current;
                readEditors(current);
                addToPipeline(current);
                dialog.reject();
            });
        }
        layout->addWidget(buttons);

        if (dialog.exec() != QDialog::Accepted)
        {
            return false;
        }
        readEditors(fields);
        return true;
    }
}
