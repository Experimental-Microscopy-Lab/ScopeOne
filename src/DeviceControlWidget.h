#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace scopeone::core
{
    class ImageSceneModel;
    class ScopeOneCore;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTableWidget;

namespace scopeone::ui
{
    class ImageWorkspace;
    class LayerHistogramWidget;
    class PreviewWidget;

    class DeviceControlWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit DeviceControlWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~DeviceControlWidget() override = default;

        void setControlTargets(const QStringList& cameraIds);

        void setImageWorkspace(ImageWorkspace* workspace);
        void setPreviewWidget(PreviewWidget* previewWidget);
        void setViewerContext(bool liveViewer);
        QWidget* imageControlsWidget() const;
        QWidget* hardwareControlsWidget() const;

        void setControlTargetEnabled(bool enabled);
        void setControlsEnabled(bool enabled);

        void refreshStageDevices();
        void refreshCameraParameters();

        void setPreviewRunning(bool running);
        void onCameraInitialized(bool initialized);
        void moveXYStep(double dxScale, double dyScale, bool big = false);
        void moveZStep(double dzScale, bool big = false);

    signals :
        void startPreviewRequested();

        void stopPreviewRequested();

        void exposureValueChanged(double exposureMs);
        void controlTargetChanged(const QString& target);
        void snapRequested(const QString& target);
        void stageMoveFailed(const QString& message);

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
        void rebuildPreviewLayerTable(const QStringList& layerKeys);
        void applyPreviewVisibility(const QStringList& layerKeys, bool notifyPreview);
        void refreshPreviewLayerSettings();
        QString selectedLayerSourceId() const;
        void onPreviewAvailableCameraIdsChanged(const QStringList& cameraIds);
        void onPreviewAvailableLayerKeysChanged(const QStringList& layerKeys);
        void onPreviewLayerInfoTextChanged(const QString& text);
        void refreshPreviewLayerInfoText();
        void onPreviewLayerVisibleToggled(bool checked);
        void onPreviewLayerOpacityChanged(int value);
        void onPreviewLayerGammaChanged(double value);
        void onPreviewLayerColormapChanged(int index);
        void onPreviewLayerBlendingChanged(int index);
        void onPreviewLayerAutoClicked();
        void onPreviewLayerFullRangeClicked();
        void onPreviewLayerAutoStretchToggled(bool enabled);
        void onPreviewLayerSelectionChanged(int currentRow,
                                            int currentColumn,
                                            int previousRow,
                                            int previousColumn);
        void onPreviewLayerMoveUpClicked();
        void onPreviewLayerMoveDownClicked();
        void onPreviewLayerRemoveClicked();
        void onLayerHistogramReady(const QString& layerKey,
                                   const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void refreshLayerHistogram();
        void syncControlTargetToSelectedRawLayer();
        void resetSelectedLayerTransform();
        void syncLayerSelection();
        QString currentLayerKey() const;

        void setupUI();

        QWidget* createControlGroup();

        void updateControlsState();

        void updateCameraParametersFromHardware();
        bool isAllTarget(const QString& target) const;
        QString roiCameraTarget() const;
        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        scopeone::core::ImageSceneModel* m_sceneModel{nullptr};
        QScrollArea* m_imageControlsWidget{nullptr};
        QScrollArea* m_hardwareControlsWidget{nullptr};
        QGroupBox* m_previewControlsGroup{nullptr};
        QGroupBox* m_cameraControlsGroup{nullptr};
        QGroupBox* m_stageControlsGroup{nullptr};
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
        QPushButton* m_layerAutoButton{nullptr};
        QPushButton* m_layerFullRangeButton{nullptr};
        QCheckBox* m_layerAutoStretchCheckBox{nullptr};
        QCheckBox* m_clippingCheckBox{nullptr};
        QCheckBox* m_scaleBarCheckBox{nullptr};
        LayerHistogramWidget* m_layerHistogramWidget{nullptr};
        QCheckBox* m_layerHistogramLogCheckBox{nullptr};
        QGroupBox* m_layerHistogramGroup{nullptr};
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
        QLabel* m_exposureLabel{nullptr};

        QPushButton* m_previewToggleButton{nullptr};
        QPushButton* m_snapButton{nullptr};
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
        QSet<quint64> m_pendingStageMoveIds;

        bool m_cameraInitialized;
        bool m_previewRunning;
        bool m_liveViewerContext{true};
        bool m_controlTargetEnabled{true};
        QString m_currentTarget;
        double m_minExposureMs{0.1};
        double m_maxExposureMs{10000.0};
    };
}
