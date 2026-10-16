#pragma once

#include "ImageOps.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

class QTableWidget;

namespace scopeone::ui
{
    // The table of measurements, like the Results window of ImageJ. The columns it shows are
    // chosen with Set Measurements and kept between runs.
    class ResultsWindow : public QWidget
    {
        Q_OBJECT

    public:
        explicit ResultsWindow(QWidget* parent = nullptr);

        // Adds a row for one measured selection. slice counts from one, and is zero without a stack.
        void addMeasurement(const QString& label, const imageops::Measurement& measurement, int slice,
                            const QString& unit);
        // Adds a row of plain values, for example the intensity at one slice of a stack
        void addValues(const QStringList& headers, const QList<double>& values);
        void chooseMeasurements();

    private:
        struct Row
        {
            QString label;
            imageops::Measurement measurement;
            int slice{0};
            QString unit;
        };
        struct Column
        {
            QString key;
            QString title;
        };
        static QList<Column> allColumns();
        QStringList enabledKeys() const;
        QString cell(const Row& row, const QString& key) const;
        void rebuild();
        QString asText(const QString& separator) const;
        void saveCsv();

        QTableWidget* m_table{nullptr};
        QList<Row> m_rows;
        // Rows that are not selection measurements
        QStringList m_plainHeaders;
        QList<QList<double>> m_plainRows;
    };
}
