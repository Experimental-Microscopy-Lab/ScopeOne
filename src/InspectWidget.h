#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QColor;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace scopeone::ui
{
    class ImageWorkspace;
    class InspectCrossSectionWidget;
    class InspectHistogramWidget;

    class InspectWidget : public QWidget
    {
        Q_OBJECT

    public:
        struct LayerInspectState
        {
            QString layerKey;
            scopeone::core::ScopeOneCore::HistogramStats stats;
            bool hasStats{false};
        };

        explicit InspectWidget(scopeone::core::ScopeOneCore* core,
                               ImageWorkspace* workspace,
                               QWidget* parent = nullptr);
        ~InspectWidget() override;

        void onCameraInitialized(bool initialized);
        void setAvailableLayers(const QStringList& layerKeys);
        void setAvailableCameras(const QStringList& cameraIds);
        void setLayerInspect(const QString& layerKey,
                             const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void clearLayerInspect(const QString& layerKey);
        void clearCrossSectionProfile();
        void setLayerCrossSectionProfile(const QString& layerKey, const QVector<int>& values);
        void setMeasurementLine(const QString& layerKey,
                                const QPoint& start,
                                const QPoint& end,
                                double actualLengthUm);
        void clearMeasurementLine();
        void refreshActiveViewer();

    signals:
        void requestDrawCrossSectionLayer(const QString& layerKey);
        void requestClearCrossSection();
        void requestDrawMeasurementLine(const QString& layerKey);
        void requestClearMeasurementLines(const QString& layerKey);

    private:
        struct LayerInfoGroup
        {
            QString layerKey;
            QGroupBox* groupBox{nullptr};
            InspectHistogramWidget* histogramWidget{nullptr};
            QCheckBox* logScaleCheckBox{nullptr};
            QSlider* minSlider{nullptr};
            QSlider* maxSlider{nullptr};
            QLabel* minSliderValueLabel{nullptr};
            QLabel* maxSliderValueLabel{nullptr};
            QLabel* meanLabel{nullptr};
            QLabel* minLabel{nullptr};
            QLabel* maxLabel{nullptr};
            QLabel* stdDevLabel{nullptr};
            QLabel* pixelCountLabel{nullptr};
        };

        struct ViewerInspectState
        {
            QHash<QString, LayerInspectState> layerStates;
            QString crossSectionLayerKey;
            QVector<int> crossSectionValues;
            QString measurementLayerKey;
            QString measurementInfo;
        };

        void setupUI();
        QWidget* createLayerInfoGroup(const QString& layerKey);
        QWidget* createStatisticsGroup(LayerInfoGroup& infoGroup);
        void addLayerInfo(const QString& layerKey);
        void removeLayerInfo(const QString& layerKey);
        void updateLayerInspect(const QString& layerKey,
                                const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void onLogScaleChanged(const QString& layerKey, bool checked);
        void updateStatisticsDisplay(const QString& layerKey,
                                     const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void updateControlsState();
        void updateLayerVisibility();
        void requestVisibleHistograms();
        void saveViewerState();
        void restoreViewerState();
        QString currentLayerKey() const;
        LayerInspectState& getOrCreateLayerState(const QString& layerKey);
        QColor getLayerColor(const QString& layerKey) const;
        void onLayerSliderChanged(const QString& layerKey, int minValue, int maxValue);
        QString currentLayerCameraId() const;

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        scopeone::core::ImageSceneModel* m_sceneModel{nullptr};
        QHash<QString, LayerInfoGroup> m_layerInfoGroups;
        QHash<QString, LayerInspectState> m_layerStates;
        QStringList m_availableLayerKeys;
        QStringList m_availableCameraIds;
        QVBoxLayout* m_histogramContainerLayout{nullptr};
        QCheckBox* m_compareLayersCheckBox{nullptr};
        QPushButton* m_drawMeasurementLineButton{nullptr};
        QPushButton* m_clearMeasurementLinesButton{nullptr};
        QLabel* m_measurementInfoLabel{nullptr};
        InspectCrossSectionWidget* m_crossSectionWidget{nullptr};
        QGroupBox* m_crossSectionGroup{nullptr};
        QPushButton* m_drawCrossSectionButton{nullptr};
        QPushButton* m_clearCrossSectionButton{nullptr};
        bool m_cameraInitialized{false};
        QString m_measurementLayerKey;
        QString m_crossSectionLayerKey;
        QHash<QString, ViewerInspectState> m_viewerStates;
        QString m_activeViewerStateId;
        QStringList m_visibleLayerKeys;
        quint64 m_histogramRequestGeneration{0};
        bool m_inspectingLive{true};
    };
}
