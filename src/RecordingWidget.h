#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QWidget>
#include <QStringList>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QTimer;

namespace scopeone::ui
{
    class RecordingWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit RecordingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~RecordingWidget() override;

        void setAvailableCameras(const QStringList& cameraIds);

    signals:
        void gallerySessionCaptured(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);

    private:
        void onBrowseClicked();
        void onAutoNameClicked();
        void onStartStopClicked();

        void setupUI();
        void updateUiState();
        void updateStorageStatus();
        void updateStorageStatusText(qint64 availableBytes);
        void moveOrderItem(int delta);
        void syncOrderList();
        bool appendSelectedFramesToGallery();

        QString getLastSaveDirectory() const;
        void setLastSaveDirectory(const QString& path);
        QString buildTimestampBaseName() const;
        QString normalizedBaseName() const;

        QStringList selectedCameraIds() const;
        bool startRecording();

    private:
        QComboBox* m_detectorCombo{nullptr};
        QLineEdit* m_saveDirLineEdit{nullptr};
        QPushButton* m_browseButton{nullptr};
        QLineEdit* m_fileNameLineEdit{nullptr};
        QPushButton* m_autoNameButton{nullptr};
        QPushButton* m_snapToGalleryButton{nullptr};

        QCheckBox* m_compressionCheck{nullptr};
        QSpinBox* m_compressionLevelSpin{nullptr};
        QComboBox* m_formatCombo{nullptr};

        QSpinBox* m_framesSpin{nullptr};
        QDoubleSpinBox* m_mdaIntervalSpin{nullptr};
        QListWidget* m_mdaOrderList{nullptr};
        QPushButton* m_mdaOrderUpButton{nullptr};
        QPushButton* m_mdaOrderDownButton{nullptr};
        QCheckBox* m_mdaEnableZCheck{nullptr};
        QDoubleSpinBox* m_mdaZStartSpin{nullptr};
        QDoubleSpinBox* m_mdaZStepSpin{nullptr};
        QSpinBox* m_mdaZCountSpin{nullptr};
        QCheckBox* m_mdaEnableXYCheck{nullptr};
        QDoubleSpinBox* m_mdaXStartSpin{nullptr};
        QDoubleSpinBox* m_mdaXStepSpin{nullptr};
        QSpinBox* m_mdaXCountSpin{nullptr};
        QDoubleSpinBox* m_mdaYStartSpin{nullptr};
        QDoubleSpinBox* m_mdaYStepSpin{nullptr};
        QSpinBox* m_mdaYCountSpin{nullptr};

        QCheckBox* m_burstModeCheck{nullptr};
        QSpinBox* m_burstCountSpin{nullptr};
        QDoubleSpinBox* m_burstIntervalSpin{nullptr};
        QComboBox* m_burstIntervalUnitCombo{nullptr};

        QPushButton* m_startStopButton{nullptr};
        QLabel* m_statusLabel{nullptr};
        QLabel* m_mdaStatusLabel{nullptr};
        QLabel* m_writerStatusLabel{nullptr};
        QLabel* m_frameCountLabel{nullptr};
        QLabel* m_burstCountLabel{nullptr};
        QLabel* m_storageStatusLabel{nullptr};
        QTimer* m_storageStatusTimer{nullptr};

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QStringList m_availableCameraIds;
        std::vector<int> m_orderPreference;
        bool m_isRecording{false};
        bool m_writerFinalizing{false};
        bool m_storageQueryPending{false};
    };
}
