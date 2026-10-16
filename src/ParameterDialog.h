#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

namespace scopeone::ui
{
    // One entry of a small dialog that asks for the settings of an operation
    struct ParameterField
    {
        enum class Kind { Integer, Real, Choice, Check };

        Kind kind{Kind::Real};
        QString label;
        double value{0.0};
        double minimum{-1.0e9};
        double maximum{1.0e9};
        int decimals{2};
        QString suffix;
        QStringList choices;
        int choice{0};
        bool checked{false};

        static ParameterField integer(const QString& label, int value, int minimum, int maximum)
        {
            ParameterField field;
            field.kind = Kind::Integer;
            field.label = label;
            field.value = value;
            field.minimum = minimum;
            field.maximum = maximum;
            return field;
        }
        static ParameterField real(const QString& label, double value, double minimum, double maximum, int decimals = 2)
        {
            ParameterField field;
            field.kind = Kind::Real;
            field.label = label;
            field.value = value;
            field.minimum = minimum;
            field.maximum = maximum;
            field.decimals = decimals;
            return field;
        }
        static ParameterField options(const QString& label, const QStringList& choices, int selected = 0)
        {
            ParameterField field;
            field.kind = Kind::Choice;
            field.label = label;
            field.choices = choices;
            field.choice = selected;
            return field;
        }
        static ParameterField check(const QString& label, bool checked)
        {
            ParameterField field;
            field.kind = Kind::Check;
            field.label = label;
            field.checked = checked;
            return field;
        }
    };

    // Makes the picture that the preview of the dialog shows for the current values of the fields
    using PreviewRenderer = std::function<QImage(const QList<ParameterField>&)>;
    using FieldsAction = std::function<void(const QList<ParameterField>&)>;

    // Shows the fields in a dialog. The values the user chose are written back to the fields.
    // Returns false when the dialog was canceled. With a preview renderer the dialog shows its
    // picture and keeps it up to date while the values change. With addToPipeline the dialog has a
    // button that hands the current values to the action and closes the dialog without a result.
    bool askParameters(QWidget* parent, const QString& title, QList<ParameterField>& fields,
                       const PreviewRenderer& preview = {}, const FieldsAction& addToPipeline = {});
}
