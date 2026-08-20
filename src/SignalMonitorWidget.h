#pragma once

#include "scopeone/SignalSource.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSpinBox;

namespace scopeone::core
{
    class ScopeOneCore;
}

namespace scopeone::ui
{
    class SignalTracePlot;

    class SignalMonitorWidget final : public QWidget
    {
        Q_OBJECT

    public:
        explicit SignalMonitorWidget(scopeone::core::ScopeOneCore* core,
                                     QWidget* parent = nullptr);

    private:
        void refreshSources();
        void rebuildSourceParameters();
        void startAcquisition();
        void handleTimeSeries(const scopeone::core::TimeSeriesChunk& chunk);
        void handleStateChanged(const QString& sourceId,
                                scopeone::core::SignalSourceState state,
                                const QString& message);
        void setSourceControlsEnabled(bool enabled);
        scopeone::core::SignalSourceDescriptor currentDescriptor() const;
        QVariantMap sourceSettings() const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        QComboBox* m_sourceCombo{nullptr};
        QFormLayout* m_sourceForm{nullptr};
        QHash<QString, QWidget*> m_parameterEditors;
        QDoubleSpinBox* m_sampleIntervalSpin{nullptr};
        QDoubleSpinBox* m_windowDurationSpin{nullptr};
        QCheckBox* m_scanImageCheck{nullptr};
        QSpinBox* m_scanWidthSpin{nullptr};
        QSpinBox* m_scanHeightSpin{nullptr};
        QSpinBox* m_scanGainSpin{nullptr};
        QComboBox* m_frameStartMarkerCombo{nullptr};
        QComboBox* m_lineStartMarkerCombo{nullptr};
        QComboBox* m_lineEndMarkerCombo{nullptr};
        QComboBox* m_frameEndMarkerCombo{nullptr};
        QCheckBox* m_serpentineCheck{nullptr};
        QPushButton* m_startButton{nullptr};
        QPushButton* m_stopButton{nullptr};
        QLabel* m_valueLabel{nullptr};
        QLabel* m_statusLabel{nullptr};
        SignalTracePlot* m_plot{nullptr};
        QString m_activeSourceId;
    };
}
