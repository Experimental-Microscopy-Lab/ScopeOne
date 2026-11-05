#pragma once

#include "OscilloscopeView.h"
#include "scopeone/SignalSource.h"
#include "tools/ToolContext.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

#include <array>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace scopeone::core
{
    class ScopeOneCore;
}

namespace scopeone::ui
{
    // The signal sources shown as an oscilloscope: each of the four channels shows one source,
    // and Start runs the sources of the channels that are on
    class SignalMonitorWidget final : public QWidget
    {
        Q_OBJECT

    public:
        explicit SignalMonitorWidget(scopeone::core::ScopeOneCore* core,
                                     scopeone::ui::ScopeOneToolContext& context,
                                     QWidget* parent = nullptr);
        ~SignalMonitorWidget() override;

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        struct ChannelControls
        {
            QGroupBox* box{nullptr};
            QComboBox* source{nullptr};
            QComboBox* scale{nullptr};
            QDoubleSpinBox* offset{nullptr};
            QComboBox* coupling{nullptr};
            QFormLayout* parameterForm{nullptr};
            QHash<QString, QWidget*> editors;
            QString preferredSource;
        };

        QWidget* createChannelBox(int index);
        void refreshSources();
        void rebuildParameters(int index);
        void updateUnits();
        void applyView();
        void startAcquisition();
        void stopAcquisition();
        void autoScale();
        void handleTimeSeries(const scopeone::core::TimeSeriesChunk& chunk);
        void handleStateChanged(const QString& sourceId,
                                scopeone::core::SignalSourceState state,
                                const QString& message);
        void updateControls();
        void updateStatus();
        void loadSettings();
        void saveSettings() const;
        scopeone::core::SignalSourceDescriptor descriptor(const QString& sourceId) const;
        QString channelSource(int index) const;
        QVariantMap sourceSettings(int index) const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        scopeone::ui::ScopeOneToolContext& m_context;
        OscilloscopeView* m_view{nullptr};
        std::array<ChannelControls, OscilloscopeView::kChannelCount> m_channels;
        QList<scopeone::core::SignalSourceDescriptor> m_sources;

        QDoubleSpinBox* m_sampleIntervalSpin{nullptr};
        QPushButton* m_startButton{nullptr};
        QPushButton* m_stopButton{nullptr};
        QLabel* m_statusLabel{nullptr};

        QComboBox* m_timebaseCombo{nullptr};
        QDoubleSpinBox* m_triggerPositionSpin{nullptr};
        QComboBox* m_triggerModeCombo{nullptr};
        QComboBox* m_triggerChannelCombo{nullptr};
        QComboBox* m_triggerSlopeCombo{nullptr};
        QDoubleSpinBox* m_triggerLevelSpin{nullptr};
        QPushButton* m_runButton{nullptr};
        QPushButton* m_singleButton{nullptr};
        QPushButton* m_autoScaleButton{nullptr};
        QCheckBox* m_cursorsCheck{nullptr};
        QComboBox* m_cursorChannelCombo{nullptr};

        QGroupBox* m_scanGroup{nullptr};
        QSpinBox* m_scanWidthSpin{nullptr};
        QSpinBox* m_scanHeightSpin{nullptr};
        QSpinBox* m_scanGainSpin{nullptr};
        QSpinBox* m_scanAverageFramesSpin{nullptr};
        QComboBox* m_frameStartMarkerCombo{nullptr};
        QComboBox* m_lineMarkerCombo{nullptr};
        QComboBox* m_frameEndMarkerCombo{nullptr};
        QCheckBox* m_serpentineCheck{nullptr};
        QCheckBox* m_mirrorHorizontalCheck{nullptr};

        QStringList m_activeSources;
        QHash<QString, QString> m_sourceMessages;
        QString m_scanSourceId;
        bool m_scanLayerShown{false};
    };
}
