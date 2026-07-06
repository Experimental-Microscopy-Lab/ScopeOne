#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace scopeone::core
{
    class ScopeOneCore;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace scopeone::ui
{
    class PreviewWidget;

    class DeviceControlWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit DeviceControlWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~DeviceControlWidget() override = default;

        void setControlTargets(const QStringList& cameraIds);

        bool acceptsFrameFromCamera(const QString& cameraId) const;

        void setPreviewWidget(PreviewWidget* previewWidget);

        void setControlTargetEnabled(bool enabled);

        void refreshStageDevices();
        void refreshCameraParameters();

        void onCameraInitialized(bool initialized);

        void setPreviewRunning(bool running);
        QString currentLayerKey() const;

    signals :
        void startPreviewRequested();

        void stopPreviewRequested();

        void exposureValueChanged(double exposureMs);
        void controlTargetChanged(const QString& target);
        void currentLayerChanged(const QString& layerKey);

        void requestDrawROI(const QString& cameraId);
        void requestHalfROI(const QString& cameraId);
        void requestClearROI(const QString& cameraId);

    private:
        void onExposureChanged();
        void onPreviewToggleClicked();

        void onControlTargetSelectionChanged(const QString& target);

        void onDrawROIClicked();

        void onHalfROIClicked();

        void onClearROIClicked();

        QWidget* createPreviewControlsGroup();
        void updatePreviewZoomControls();
        void rebuildPreviewLayerTable(const QStringList& layerKeys);
        void applyPreviewSelection(const QStringList& layerKeys, bool notifyPreview);
        void refreshPreviewLayerSettings();
        QString selectedLayerSourceId() const;
        void onPreviewAvailableCameraIdsChanged(const QStringList& cameraIds);
        void onPreviewAvailableLayerKeysChanged(const QStringList& layerKeys);
        void syncPreviewLayerLayoutCombo(int index);
        void onPreviewLayerInfoTextChanged(const QString& text);
        void refreshPreviewLayerInfoText();

        void onPreviewZoomSpinBoxChanged(int value);
        void onPreviewFitToWindowToggled(bool enabled);
        void onPreviewLayerLayoutComboChanged(int index);
        void onPreviewLayerVisibleToggled(bool checked);
        void onPreviewLayerOpacityChanged(int value);
        void onPreviewLayerGammaChanged(double value);
        void onPreviewLayerColormapChanged(int index);
        void onPreviewLayerBlendingChanged(int index);
        void onPreviewLayerSelectionChanged(int currentRow, int currentColumn, int previousRow, int previousColumn);
        void onPreviewLayerMoveUpClicked();
        void onPreviewLayerMoveDownClicked();
        void onPreviewLayerRemoveClicked();
        void resetSelectedLayerTransform();

        void setupUI();

        QWidget* createControlGroup();

        void updateControlsState();

        void updateCameraParametersFromHardware();
        bool isAllTarget(const QString& target) const;
        QString roiCameraTarget() const;
        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QGroupBox* m_previewControlsGroup{nullptr};
        QLabel* m_zoomLabel{nullptr};
        QSpinBox* m_zoomSpinBox{nullptr};
        QCheckBox* m_fitToWindowCheckBox{nullptr};
        QComboBox* m_layerLayoutCombo{nullptr};
        QTableWidget* m_layerTable{nullptr};
        QMap<QString, QCheckBox*> m_layerRows;
        QGroupBox* m_layerSettingsGroup{nullptr};
        QLabel* m_selectedLayerLabel{nullptr};
        QPushButton* m_layerMoveUpButton{nullptr};
        QPushButton* m_layerMoveDownButton{nullptr};
        QPushButton* m_layerRemoveButton{nullptr};
        QSpinBox* m_layerOpacitySpinBox{nullptr};
        QDoubleSpinBox* m_layerGammaSpinBox{nullptr};
        QComboBox* m_layerColormapComboBox{nullptr};
        QComboBox* m_layerBlendingComboBox{nullptr};
        QString m_selectedLayerKey;
        QLabel* m_alignXLabel{nullptr};
        QSpinBox* m_alignXSpinBox{nullptr};
        QLabel* m_alignYLabel{nullptr};
        QSpinBox* m_alignYSpinBox{nullptr};
        QLabel* m_alignZoomLabel{nullptr};
        QSpinBox* m_alignZoomSpinBox{nullptr};
        QCheckBox* m_alignFlipXCheckBox{nullptr};
        QCheckBox* m_alignFlipYCheckBox{nullptr};
        QPushButton* m_alignResetButton{nullptr};
        PreviewWidget* m_previewWidget{nullptr};

        QLineEdit* m_exposureLineEdit{nullptr};

        QPushButton* m_previewToggleButton{nullptr};
        QComboBox* m_cameraSelectCombo{nullptr};
        QPushButton* m_drawROIButton{nullptr};
        QPushButton* m_halfROIButton{nullptr};
        QPushButton* m_clearROIButton{nullptr};

        QWidget* createStageGroup();
        void updateStageControlsEnabled();
        void updateStagePositions();
        QString selectedXYStageLabel() const;
        QString selectedZStageLabel() const;
        void moveXYStage(double dx, double dy);
        void moveZStage(double dz);
        void updateExposureLimits();

        QComboBox* m_xyStageCombo{nullptr};
        QComboBox* m_zStageCombo{nullptr};
        QLineEdit* m_xyStepLineEdit{nullptr};
        QLineEdit* m_xyBigStepLineEdit{nullptr};
        QLineEdit* m_zStepLineEdit{nullptr};
        QLineEdit* m_zBigStepLineEdit{nullptr};
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
        double m_minExposureMs{0.1};
        double m_maxExposureMs{10000.0};
    };
}
