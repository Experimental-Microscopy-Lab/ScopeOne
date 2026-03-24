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

namespace scopeone::ui {

class RecordingWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RecordingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~RecordingWidget() override;

    void setAvailableCameras(const QStringList& cameraIds);

private:

    void bindCoreSignals();
    void onBrowseClicked();
    void onAutoNameClicked();
    void onStartStopClicked();
    void onBurstModeToggled(bool enabled);
    void onDetectorChanged(const QString& text);
    void onToAlbumClicked();
    void onAlbumClicked();
    void onClearAlbumClicked();

    void setupUI();
    void updateUiState();
    void moveOrderItem(int delta);
    void syncOrderList();
    void updateAlbumState();
    bool appendSelectedFramesToAlbum();
    int albumFrameCount() const;
    QString albumBaseName() const;
    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> buildAlbumPreviewSession() const;
    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> buildAlbumSaveSession() const;

    QString getLastSaveDirectory() const;
    void setLastSaveDirectory(const QString& path);
    QString buildTimestampBaseName() const;
    QString normalizedBaseName() const;

    QStringList selectedCameraIds() const;
    bool startRecording();
    void stopRecording();

private:

    QComboBox* m_detectorCombo{nullptr};
    QLineEdit* m_saveDirLineEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QLineEdit* m_fileNameLineEdit{nullptr};
    QPushButton* m_autoNameButton{nullptr};
    QPushButton* m_toAlbumButton{nullptr};
    QPushButton* m_albumButton{nullptr};
    QPushButton* m_clearAlbumButton{nullptr};
    QLabel* m_albumCountLabel{nullptr};


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
    QLabel* m_writerStatusLabel{nullptr};
    QLabel* m_frameCountLabel{nullptr};
    QLabel* m_burstCountLabel{nullptr};


    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    bool m_coreSignalsBound{false};
    QStringList m_availableCameraIds;
    std::vector<int> m_orderPreference;
    bool m_isRecording{false};
    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> m_albumSession;
};

}
