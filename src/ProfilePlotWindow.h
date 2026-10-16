#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace scopeone::ui
{
    // A window that plots a list of values against their position, for example the intensity of
    // a selection at each slice of a stack
    class ProfilePlotWindow : public QWidget
    {
        Q_OBJECT

    public:
        explicit ProfilePlotWindow(QWidget* parent = nullptr);
        void setProfile(const QString& title, const QString& xLabel, const QString& yLabel,
                        const QVector<double>& values);

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        void saveCsv();

        QString m_title;
        QString m_xLabel;
        QString m_yLabel;
        QVector<double> m_values;
    };
}
