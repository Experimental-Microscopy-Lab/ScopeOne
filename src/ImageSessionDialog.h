#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QDialog>
#include <QHash>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

namespace scopeone::ui {

class InspectWidget;
class PreviewWidget;

class ImageSessionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImageSessionDialog(QWidget* parent = nullptr);
    explicit ImageSessionDialog(std::shared_ptr<const scopeone::core::ScopeOneCore::RecordingSessionData> session,
                                QWidget* parent = nullptr);

    void setRecordingSession(std::shared_ptr<const scopeone::core::ScopeOneCore::RecordingSessionData> session);
    void setLiveFrame(const scopeone::core::ImageFrame& frame);
    void clearLiveFrames();

    void setSaveEnabled(bool enabled);
    void setSaveButtonText(const QString& text);
    bool saveRequested() const { return m_saveRequested; }

private:
    void setupUI();
    void rebuildCameraList();
    void updateFrameControls();
    void updateDisplayedFrame();
    int currentFrameCount() const;
    QString currentCameraId() const;
    scopeone::core::ImageFrame normalizedFrame(
        const scopeone::core::ScopeOneCore::RecordingFrame& frame,
        const QString& cameraId) const;
    void showPreviewFrame(const scopeone::core::ImageFrame& frame);

    std::shared_ptr<const scopeone::core::ScopeOneCore::RecordingSessionData> m_session;
    QHash<QString, scopeone::core::ImageFrame> m_liveFrames;
    bool m_saveRequested{false};
    bool m_saveEnabledByOwner{true};
    QString m_lastPreviewCameraId;

    QComboBox* m_cameraCombo{nullptr};
    QSlider* m_frameSlider{nullptr};
    QSpinBox* m_frameSpin{nullptr};
    QLabel* m_frameInfoLabel{nullptr};
    QCheckBox* m_fitToWindowCheck{nullptr};
    QSlider* m_zoomSlider{nullptr};
    QLabel* m_zoomLabel{nullptr};
    PreviewWidget* m_previewWidget{nullptr};
    InspectWidget* m_inspectWidget{nullptr};
    QPushButton* m_saveButton{nullptr};
    QPushButton* m_closeButton{nullptr};
};

}
