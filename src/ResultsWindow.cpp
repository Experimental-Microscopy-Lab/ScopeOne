#include "ResultsWindow.h"
#include "EmptyHint.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace scopeone::ui
{
    QList<ResultsWindow::Column> ResultsWindow::allColumns()
    {
        return {
            {QStringLiteral("label"), QStringLiteral("Label")},
            {QStringLiteral("area"), QStringLiteral("Area")},
            {QStringLiteral("mean"), QStringLiteral("Mean")},
            {QStringLiteral("stddev"), QStringLiteral("StdDev")},
            {QStringLiteral("median"), QStringLiteral("Median")},
            {QStringLiteral("min"), QStringLiteral("Min")},
            {QStringLiteral("max"), QStringLiteral("Max")},
            {QStringLiteral("centroid"), QStringLiteral("X, Y (centroid)")},
            {QStringLiteral("perimeter"), QStringLiteral("Perim.")},
            {QStringLiteral("bounds"), QStringLiteral("BX, BY, Width, Height")},
            {QStringLiteral("intden"), QStringLiteral("IntDen")},
            {QStringLiteral("rawintden"), QStringLiteral("RawIntDen")},
            {QStringLiteral("skewness"), QStringLiteral("Skew")},
            {QStringLiteral("kurtosis"), QStringLiteral("Kurt")},
            {QStringLiteral("shape"), QStringLiteral("Circ., AR, Round, Solidity")},
            {QStringLiteral("feret"), QStringLiteral("Feret")},
            {QStringLiteral("ellipse"), QStringLiteral("Major, Minor")},
            {QStringLiteral("slice"), QStringLiteral("Slice")},
        };
    }

    ResultsWindow::ResultsWindow(QWidget* parent)
        : QWidget(parent, Qt::Window)
    {
        setWindowTitle(tr("Results"));
        resize(760, 360);
        auto* layout = new QVBoxLayout(this);
        auto* buttons = new QHBoxLayout();
        auto* copyButton = new QPushButton(tr("Copy"), this);
        auto* saveButton = new QPushButton(tr("Save as CSV..."), this);
        auto* clearButton = new QPushButton(tr("Clear"), this);
        auto* measurementsButton = new QPushButton(tr("Set Measurements..."), this);
        for (QPushButton* button : {copyButton, saveButton, clearButton, measurementsButton})
        {
            buttons->addWidget(button);
        }
        buttons->addStretch();
        layout->addLayout(buttons);

        m_table = new QTableWidget(this);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->horizontalHeader()->setStretchLastSection(true);
        new EmptyHint(m_table, tr("No measurements yet. Choose Analyze > Measure."));
        layout->addWidget(m_table);

        connect(copyButton, &QPushButton::clicked, this, [this]()
        {
            QApplication::clipboard()->setText(asText(QStringLiteral("\t")));
        });
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveCsv(); });
        connect(clearButton, &QPushButton::clicked, this, [this]()
        {
            m_rows.clear();
            m_plainRows.clear();
            m_plainHeaders.clear();
            rebuild();
        });
        connect(measurementsButton, &QPushButton::clicked, this, [this]() { chooseMeasurements(); });
        rebuild();
    }

    QStringList ResultsWindow::enabledKeys() const
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QStringList defaults{QStringLiteral("label"), QStringLiteral("area"), QStringLiteral("mean"),
                                   QStringLiteral("min"), QStringLiteral("max")};
        return settings.value(QStringLiteral("Measurements/columns"), defaults).toStringList();
    }

    void ResultsWindow::chooseMeasurements()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Set Measurements"));
        auto* layout = new QVBoxLayout(&dialog);
        const QStringList enabled = enabledKeys();
        QList<QCheckBox*> boxes;
        for (const Column& column : allColumns())
        {
            auto* box = new QCheckBox(column.title, &dialog);
            box->setChecked(enabled.contains(column.key));
            layout->addWidget(box);
            boxes.append(box);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
        QStringList chosen;
        const QList<Column> columns = allColumns();
        for (qsizetype index = 0; index < boxes.size(); ++index)
        {
            if (boxes.at(index)->isChecked())
            {
                chosen.append(columns.at(index).key);
            }
        }
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        settings.setValue(QStringLiteral("Measurements/columns"), chosen);
        rebuild();
    }

    void ResultsWindow::addMeasurement(const QString& label, const imageops::Measurement& measurement,
                                       int slice, const QString& unit, const QString& position)
    {
        // Rows of another kind are not mixed into one table
        if (!m_plainRows.isEmpty())
        {
            m_plainRows.clear();
            m_plainHeaders.clear();
        }
        m_rows.append({label, measurement, slice, unit, position});
        rebuild();
        emit attentionRequested();
    }

    void ResultsWindow::addValues(const QStringList& headers, const QList<double>& values)
    {
        if (!m_rows.isEmpty() || m_plainHeaders != headers)
        {
            m_rows.clear();
            m_plainRows.clear();
        }
        m_plainHeaders = headers;
        m_plainRows.append(values);
        rebuild();
        emit attentionRequested();
    }

    QString ResultsWindow::cell(const Row& row, const QString& key) const
    {
        const imageops::Measurement& m = row.measurement;
        const auto number = [](double value) { return QString::number(value, 'g', 6); };
        const QString unit = row.unit;
        if (key == QLatin1String("label")) return row.label;
        if (key == QLatin1String("area")) return number(m.area);
        if (key == QLatin1String("mean")) return number(m.mean);
        if (key == QLatin1String("stddev")) return number(m.stdDev);
        if (key == QLatin1String("median")) return number(m.median);
        if (key == QLatin1String("min")) return number(m.minValue);
        if (key == QLatin1String("max")) return number(m.maxValue);
        if (key == QLatin1String("centroid")) return number(m.centroidX) + QStringLiteral(", ") + number(m.centroidY);
        if (key == QLatin1String("perimeter")) return number(m.perimeter);
        if (key == QLatin1String("bounds"))
            return number(m.boundsX) + QStringLiteral(", ") + number(m.boundsY) + QStringLiteral(", ")
                + number(m.boundsWidth) + QStringLiteral(", ") + number(m.boundsHeight);
        if (key == QLatin1String("intden")) return number(m.integratedDensity);
        if (key == QLatin1String("rawintden")) return number(m.rawIntegratedDensity);
        if (key == QLatin1String("skewness")) return number(m.skewness);
        if (key == QLatin1String("kurtosis")) return number(m.kurtosis);
        if (key == QLatin1String("shape"))
            return number(m.circularity) + QStringLiteral(", ") + number(m.aspectRatio) + QStringLiteral(", ")
                + number(m.roundness) + QStringLiteral(", ") + number(m.solidity);
        if (key == QLatin1String("feret")) return number(m.feret);
        if (key == QLatin1String("ellipse")) return number(m.majorAxis) + QStringLiteral(", ") + number(m.minorAxis);
        if (key == QLatin1String("slice"))
            return !row.position.isEmpty() ? row.position : (row.slice > 0 ? QString::number(row.slice) : QString());
        return {};
    }

    void ResultsWindow::rebuild()
    {
        m_table->clear();
        if (!m_plainHeaders.isEmpty())
        {
            m_table->setColumnCount(m_plainHeaders.size());
            m_table->setHorizontalHeaderLabels(m_plainHeaders);
            m_table->setRowCount(static_cast<int>(m_plainRows.size()));
            for (int row = 0; row < m_plainRows.size(); ++row)
            {
                for (int column = 0; column < m_plainHeaders.size(); ++column)
                {
                    m_table->setItem(row, column,
                                     new QTableWidgetItem(QString::number(m_plainRows.at(row).at(column), 'g', 8)));
                }
            }
            return;
        }
        const QStringList keys = enabledKeys();
        QList<Column> shown;
        for (const Column& column : allColumns())
        {
            if (keys.contains(column.key))
            {
                shown.append(column);
            }
        }
        m_table->setColumnCount(static_cast<int>(shown.size()));
        QStringList titles;
        for (const Column& column : shown)
        {
            titles.append(column.title);
        }
        m_table->setHorizontalHeaderLabels(titles);
        m_table->setRowCount(static_cast<int>(m_rows.size()));
        for (int row = 0; row < m_rows.size(); ++row)
        {
            for (int column = 0; column < shown.size(); ++column)
            {
                m_table->setItem(row, column, new QTableWidgetItem(cell(m_rows.at(row), shown.at(column).key)));
            }
        }
        m_table->scrollToBottom();
    }

    QString ResultsWindow::asText(const QString& separator) const
    {
        QString text;
        QStringList header;
        for (int column = 0; column < m_table->columnCount(); ++column)
        {
            header.append(m_table->horizontalHeaderItem(column)->text());
        }
        text += header.join(separator) + QLatin1Char('\n');
        for (int row = 0; row < m_table->rowCount(); ++row)
        {
            QStringList values;
            for (int column = 0; column < m_table->columnCount(); ++column)
            {
                QString value = m_table->item(row, column)->text();
                if (separator == QLatin1String(",") && value.contains(QLatin1Char(',')))
                {
                    value = QLatin1Char('"') + value + QLatin1Char('"');
                }
                values.append(value);
            }
            text += values.join(separator) + QLatin1Char('\n');
        }
        return text;
    }

    void ResultsWindow::saveCsv()
    {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save results"), QStringLiteral("results.csv"),
                                                          tr("CSV files (*.csv)"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream(&file) << asText(QStringLiteral(","));
        }
    }
}
