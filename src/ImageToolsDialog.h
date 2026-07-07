#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QDialog>
#include <QString>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTimer;

namespace scopeone::ui
{
    class PreviewWidget;

    class StageMosaicDialog : public QDialog
    {
        Q_OBJECT

    public:
        StageMosaicDialog(scopeone::core::ScopeOneCore* core,
                          PreviewWidget* previewWidget,
                          QWidget* parent = nullptr);
        ~StageMosaicDialog() override;

        void reject() override;

    signals:
        void gallerySessionCreated(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& title);

    private:
        void setupUI();
        void refreshDevices();
        void startMosaic();
        void stopMosaic();
        void captureNextTile();
        void captureSettledTile();
        void onRawFrameReady(const scopeone::core::ImageFrame& frame);
        void finishMosaic(const QString& message, bool addToGallery = false);
        bool initializeMosaicFrame(const scopeone::core::ImageFrame& frame);
        bool appendTileFrame(const scopeone::core::ImageFrame& frame, int row, int column);
        bool updatePreviewMosaic();
        void publishMosaicToGallery();
        QString selectedCameraId() const;
        QString selectedStageId() const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        PreviewWidget* m_previewWidget{nullptr};
        QComboBox* m_cameraCombo{nullptr};
        QComboBox* m_stageCombo{nullptr};
        QSpinBox* m_rowsSpinBox{nullptr};
        QSpinBox* m_columnsSpinBox{nullptr};
        QDoubleSpinBox* m_pixelSizeSpinBox{nullptr};
        QDoubleSpinBox* m_stepXSpinBox{nullptr};
        QDoubleSpinBox* m_stepYSpinBox{nullptr};
        QSpinBox* m_settleMsSpinBox{nullptr};
        QCheckBox* m_returnToStartCheckBox{nullptr};
        QPushButton* m_startButton{nullptr};
        QPushButton* m_stopButton{nullptr};
        QLabel* m_statusLabel{nullptr};
        QString m_activeCameraId;
        QString m_activeStageId;
        double m_originX{0.0};
        double m_originY{0.0};
        bool m_originValid{false};
        int m_currentTile{0};
        bool m_running{false};
        bool m_mosaicInitialized{false};
        bool m_waitingForTileFrame{false};
        int m_frameWaitSerial{0};
        QString m_mosaicError;
        scopeone::core::ImageFrame m_mosaicReferenceFrame;
        bool m_gallerySessionPublished{false};
        class MosaicStorage;
        std::unique_ptr<MosaicStorage> m_mosaic;
    };

    class ParticleDetectionDialog : public QDialog
    {
        Q_OBJECT

    public:
        ParticleDetectionDialog(scopeone::core::ScopeOneCore* core,
                                PreviewWidget* previewWidget,
                                QWidget* parent = nullptr);

        void reject() override;

    private:
        void setupUI();
        void refreshCameras();
        void ensurePreviewRunning(const QString& cameraId);
        void analyzeCurrentFrame();
        QString selectedCameraId() const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        PreviewWidget* m_previewWidget{nullptr};
        QComboBox* m_cameraCombo{nullptr};
        QSpinBox* m_thresholdSpinBox{nullptr};
        QSpinBox* m_minAreaSpinBox{nullptr};
        QSpinBox* m_maxAreaSpinBox{nullptr};
        QCheckBox* m_autoUpdateCheckBox{nullptr};
        QPushButton* m_analyzeButton{nullptr};
        QLabel* m_statusLabel{nullptr};
        QTimer* m_autoUpdateTimer{nullptr};
        QString m_requestedPreviewCameraId;
    };
}
