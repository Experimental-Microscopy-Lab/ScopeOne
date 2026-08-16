#pragma once

#include "scopeone/DaqDevice.h"

#include <QWidget>
#include <QElapsedTimer>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace scopeone::core
{
    class ScopeOneCore;
}

namespace scopeone::ui
{
    class DaqControlWidget final : public QWidget
    {
        Q_OBJECT

    public:
        explicit DaqControlWidget(scopeone::core::ScopeOneCore* core,
                                  QWidget* parent = nullptr);

    private:
        scopeone::core::DaqDeviceDescriptor currentDevice() const;
        QStringList terminalChoices(const scopeone::core::DaqDeviceDescriptor& device) const;
        void refreshDevice();
        void addPulseRow();
        void addBufferedRow();
        void configureAnalogWaveform(int row, QLineEdit* outputData);
        void addRouteRow();
        void removeSelectedRow(QTableWidget* table);
        void startSession();
        void setControlsEnabled(bool enabled);
        void handleStateChanged(const QString& deviceId,
                                scopeone::core::DaqState state,
                                const QString& message);

        scopeone::core::ScopeOneCore* m_core{nullptr};
        QComboBox* m_deviceCombo{nullptr};
        QLabel* m_productLabel{nullptr};
        QLabel* m_resourcesLabel{nullptr};
        QGroupBox* m_rasterGroup{nullptr};
        QComboBox* m_lineClockCombo{nullptr};
        QDoubleSpinBox* m_lineRateSpin{nullptr};
        QSpinBox* m_activeLinesSpin{nullptr};
        QSpinBox* m_flybackLinesSpin{nullptr};
        QComboBox* m_yChannelCombo{nullptr};
        QDoubleSpinBox* m_yStartSpin{nullptr};
        QDoubleSpinBox* m_yEndSpin{nullptr};
        QComboBox* m_frameCounterCombo{nullptr};
        QComboBox* m_lineOutputCombo{nullptr};
        QComboBox* m_frameOutputCombo{nullptr};
        QTableWidget* m_pulseTable{nullptr};
        QTableWidget* m_bufferedTable{nullptr};
        QTableWidget* m_routeTable{nullptr};
        QPushButton* m_addPulseButton{nullptr};
        QPushButton* m_removePulseButton{nullptr};
        QPushButton* m_addBufferedButton{nullptr};
        QPushButton* m_removeBufferedButton{nullptr};
        QPushButton* m_addRouteButton{nullptr};
        QPushButton* m_removeRouteButton{nullptr};
        QPushButton* m_startButton{nullptr};
        QPushButton* m_stopButton{nullptr};
        QLabel* m_statusLabel{nullptr};
        QString m_activeDeviceId;
        QElapsedTimer m_inputStatusTimer;
    };
}
