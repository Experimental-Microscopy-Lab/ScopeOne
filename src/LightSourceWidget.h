#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QFutureWatcher>
#include <QColor>
#include <QGroupBox>
#include <QList>
#include <QStringList>

class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;

namespace scopeone::ui
{
    // The light source channels of the cfg file, one row each with the switch, the power setting
    // and the measured power. Reads and writes of the devices run in the thread pool.
    class LightSourceWidget : public QGroupBox
    {
        Q_OBJECT

    public:
        explicit LightSourceWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);

    signals:
        void writeFailed(const QString& message);
        void editRequested();

    protected:
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

    private:
        struct Row
        {
            scopeone::core::ScopeOneCore::LightSource source;
            QPushButton* enableButton{nullptr};
            QSlider* powerSlider{nullptr};
            QDoubleSpinBox* powerSpinBox{nullptr};
            QLabel* readingLabel{nullptr};
            QString powerUnit;
            QString readingUnit;
            double lastPower{0.0};
        };

        void rebuild();
        void readRows(bool settings);
        void refreshSettings();
        void applyStates(const QList<scopeone::core::ScopeOneCore::LightSourceState>& states, bool settings);
        void updateTimer();
        void writePower(int index);
        void syncSlider(Row& row);
        void readMeasuredPower();
        void write(const QString& device, const QString& property, const QString& value);
        QString formatReading(const QString& value, const QString& unit) const;

        scopeone::core::ScopeOneCore* m_core;
        QGridLayout* m_layout{nullptr};
        QList<Row> m_rows;
        QTimer* m_timer{nullptr};
        QToolButton* m_editButton{nullptr};
        bool m_reading{false};
        int m_generation{0};
    };
}
