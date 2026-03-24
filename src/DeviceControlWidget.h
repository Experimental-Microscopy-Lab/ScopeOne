#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace scopeone::core { class ScopeOneCore; }

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace scopeone::ui {

class PreviewWidget;

class DeviceControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceControlWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~DeviceControlWidget() override = default;

    void setControlTargets(const QStringList& cameraIds);

    bool acceptsCameraStream(const QString& cameraId) const;

    void setPreviewWidget(PreviewWidget* previewWidget);
    void setPreviewSelectedChannels(const QStringList& channelIds);

    void setControlTargetEnabled(bool enabled);

    void refreshStageDevices();

    void onCameraInitialized(bool initialized);

    void setPreviewRunning(bool running);

signals:
    void startPreviewRequested();

    void stopPreviewRequested();

    void exposureValueChanged(double exposureMs);
    void controlTargetChanged(const QString& target);

    void requestDrawROI(const QString& cameraId);
    void requestClearROI(const QString& cameraId);

private:
    void onExposureChanged();
    void onPreviewToggleClicked();

    void onControlTargetSelectionChanged(const QString& target);

    void onDrawROIClicked();

    void onClearROIClicked();

    QWidget* createPreviewControlsGroup();
    void updatePreviewZoomControls();
    void rebuildPreviewChannelMenu(const QStringList& channelIds);
    void populatePreviewChannelMenuHeader();
    void updatePreviewSelectionFromActions();
    void applyPreviewSelection(const QStringList& channelIds, bool notifyPreview);
    void setPreviewChannelActionStates(const QString& selectedPrefix, bool checkedWhenPrefixEmpty);
    void onPreviewAvailableChannelsChanged(const QStringList& channelIds);
    void onPreviewSelectedChannelsChanged(const QStringList& channelIds);
    void syncPreviewChannelModeCombo(int index);
    void onPreviewInfoTextChanged(const QString& text);

    void onPreviewZoomSpinBoxChanged(int value);
    void onPreviewFitToWindowToggled(bool enabled);
    void onPreviewChannelModeComboChanged(int index);
    void onPreviewOverlayAlphaChanged(int value);
    void onPreviewChannelActionToggled(bool checked);
    void onSelectAllPreviewChannels();
    void onClearPreviewSelection();
    void onSelectAllPreviewRaw();
    void onSelectAllPreviewProcessed();
    void onAlignCameraChanged(const QString& cameraId);
    void onAlignXChanged(int x);
    void onAlignYChanged(int y);
    void onAlignZoomChanged(int percent);
    void onAlignFlipXChanged(bool enabled);
    void onAlignFlipYChanged(bool enabled);
    void onAlignResetClicked();

    void setupUI();

    QWidget* createControlGroup();

    void updateControlsState();

    void updateCameraParametersFromHardware();
    bool isAllTarget(const QString& target) const;
    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    QGroupBox* m_previewControlsGroup{nullptr};
    QLabel* m_zoomLabel{nullptr};
    QSpinBox* m_zoomSpinBox{nullptr};
    QCheckBox* m_fitToWindowCheckBox{nullptr};
    QComboBox* m_channelModeCombo{nullptr};
    QSpinBox* m_overlayAlphaSpinBox{nullptr};
    QToolButton* m_channelPickerButton{nullptr};
    QMenu* m_channelMenu{nullptr};
    QMap<QString, QAction*> m_channelActions;
    QLabel* m_alignLabel{nullptr};
    QComboBox* m_alignCameraCombo{nullptr};
    QLabel* m_alignXLabel{nullptr};
    QSpinBox* m_alignXSpinBox{nullptr};
    QLabel* m_alignYLabel{nullptr};
    QSpinBox* m_alignYSpinBox{nullptr};
    QLabel* m_alignZoomLabel{nullptr};
    QSpinBox* m_alignZoomSpinBox{nullptr};
    QCheckBox* m_alignFlipXCheckBox{nullptr};
    QCheckBox* m_alignFlipYCheckBox{nullptr};
    QPushButton* m_alignResetButton{nullptr};
    QLabel* m_imageInfoLabel{nullptr};
    PreviewWidget* m_previewWidget{nullptr};

    QLineEdit* m_exposureLineEdit{nullptr};

    QPushButton* m_previewToggleButton{nullptr};
    QComboBox* m_cameraSelectCombo{nullptr};
    QPushButton* m_drawROIButton{nullptr};
    QPushButton* m_clearROIButton{nullptr};

    QWidget* createStageGroup();
    void updateStageControlsEnabled();
    void updateStagePositions();
    QString selectedXYStageLabel() const;
    QString selectedZStageLabel() const;
    void moveXYStage(double dx, double dy);
    void moveZStage(double dz);

    QComboBox* m_xyStageCombo{nullptr};
    QComboBox* m_zStageCombo{nullptr};
    QDoubleSpinBox* m_xyStepSpinBox{nullptr};
    QDoubleSpinBox* m_xyBigStepSpinBox{nullptr};
    QDoubleSpinBox* m_zStepSpinBox{nullptr};
    QDoubleSpinBox* m_zBigStepSpinBox{nullptr};
    QLabel* m_xPosLabel{nullptr};
    QLabel* m_yPosLabel{nullptr};
    QLabel* m_zPosLabel{nullptr};
    QPushButton* m_xyUpButton{nullptr};
    QPushButton* m_xyDownButton{nullptr};
    QPushButton* m_xyLeftButton{nullptr};
    QPushButton* m_xyRightButton{nullptr};
    QPushButton* m_xyBigUpButton{nullptr};
    QPushButton* m_xyBigDownButton{nullptr};
    QPushButton* m_xyBigLeftButton{nullptr};
    QPushButton* m_xyBigRightButton{nullptr};
    QPushButton* m_zUpButton{nullptr};
    QPushButton* m_zDownButton{nullptr};
    QPushButton* m_zBigUpButton{nullptr};
    QPushButton* m_zBigDownButton{nullptr};

    bool m_cameraInitialized;
    bool m_previewRunning;
    QString m_currentTarget;
};

}
