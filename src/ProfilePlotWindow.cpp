#include "ProfilePlotWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>

namespace scopeone::ui
{
    ProfilePlotWindow::ProfilePlotWindow(QWidget* parent)
        : QWidget(parent, Qt::Window)
    {
        resize(560, 360);
        auto* layout = new QVBoxLayout(this);
        layout->addStretch();
        auto* buttons = new QHBoxLayout();
        auto* copyButton = new QPushButton(tr("Copy"), this);
        auto* saveButton = new QPushButton(tr("Save as CSV..."), this);
        buttons->addWidget(copyButton);
        buttons->addWidget(saveButton);
        buttons->addStretch();
        layout->addLayout(buttons);
        connect(copyButton, &QPushButton::clicked, this, [this]()
        {
            QString text = m_xLabel + QLatin1Char('\t') + m_yLabel + QLatin1Char('\n');
            for (qsizetype i = 0; i < m_values.size(); ++i)
            {
                text += QString::number(i + 1) + QLatin1Char('\t') + QString::number(m_values.at(i), 'g', 8) + QLatin1Char('\n');
            }
            QApplication::clipboard()->setText(text);
        });
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveCsv(); });
    }

    void ProfilePlotWindow::setProfile(const QString& title, const QString& xLabel, const QString& yLabel,
                                       const QVector<double>& values)
    {
        m_title = title;
        m_xLabel = xLabel;
        m_yLabel = yLabel;
        m_values = values;
        setWindowTitle(title);
        update();
    }

    void ProfilePlotWindow::saveCsv()
    {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save profile"), QStringLiteral("profile.csv"),
                                                          tr("CSV files (*.csv)"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream out(&file);
            out << m_xLabel << ',' << m_yLabel << '\n';
            for (qsizetype i = 0; i < m_values.size(); ++i)
            {
                out << (i + 1) << ',' << QString::number(m_values.at(i), 'g', 8) << '\n';
            }
        }
    }

    void ProfilePlotWindow::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        const QPalette& colors = palette();
        painter.fillRect(rect(), colors.brush(QPalette::Base));
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (m_values.isEmpty())
        {
            return;
        }
        const QRect plot = rect().adjusted(60, 28, -16, -80);
        painter.setPen(colors.color(QPalette::Mid));
        painter.drawRect(plot);

        double low = *std::min_element(m_values.begin(), m_values.end());
        double high = *std::max_element(m_values.begin(), m_values.end());
        if (high - low < 1e-12)
        {
            high = low + 1.0;
        }
        painter.setPen(colors.color(QPalette::Text));
        painter.drawText(QRect(8, 4, width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        painter.drawText(QRect(0, plot.top() - 6, 56, 14), Qt::AlignRight, QString::number(high, 'g', 5));
        painter.drawText(QRect(0, plot.bottom() - 8, 56, 14), Qt::AlignRight, QString::number(low, 'g', 5));
        painter.drawText(QRect(plot.left(), plot.bottom() + 4, 60, 14), Qt::AlignLeft, QStringLiteral("1"));
        painter.drawText(QRect(plot.right() - 60, plot.bottom() + 4, 60, 14), Qt::AlignRight,
                         QString::number(m_values.size()));
        painter.drawText(QRect(plot.left(), plot.bottom() + 20, plot.width(), 16), Qt::AlignCenter, m_xLabel);

        QPen pen(QColor(0, 150, 200));
        pen.setWidthF(1.6);
        painter.setPen(pen);
        QPolygonF line;
        for (qsizetype i = 0; i < m_values.size(); ++i)
        {
            const double x = m_values.size() == 1
                ? plot.center().x()
                : plot.left() + plot.width() * static_cast<double>(i) / (m_values.size() - 1);
            const double y = plot.bottom() - plot.height() * (m_values.at(i) - low) / (high - low);
            line << QPointF(x, y);
        }
        painter.drawPolyline(line);
    }
}
