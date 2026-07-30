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

        explicit InspectWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~InspectWidget() override;

        void onCameraInitialized(bool initialized);
        void setCurrentLayer(const QString& layerKey);
        void setAvailableLayers(const QStringList& layerKeys);
        void setAvailableCameras(const QStringList& cameraIds);
        void setCrossSectionVisible(bool visible);
        void setLayerInspect(const QString& layerKey,
                             const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void clearInspect();
        void clearLayerInspect(const QString& layerKey);
        void clearCrossSectionProfile();
        void setLayerCrossSectionProfile(const QString& layerKey, const QVector<int>& values);
        void setMeasurementLine(const QString& layerKey,
                                const QPoint& start,
                                const QPoint& end,
                                double pixelSizeUm);
        void clearMeasurementLine();

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
            QPushButton* autoButton{nullptr};
            QPushButton* fullButton{nullptr};
            QCheckBox* autoStretchCheckBox{nullptr};
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

        void setupUI();
        QWidget* createLayerInfoGroup(const QString& layerKey);
        QWidget* createStatisticsGroup(LayerInfoGroup& infoGroup);
        void addLayerInfo(const QString& layerKey);
        void removeLayerInfo(const QString& layerKey);
        void updateLayerInspect(const QString& layerKey,
                                const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void onAutoButtonClicked(const QString& layerKey);
        void onFullButtonClicked(const QString& layerKey);
        void onAutoStretchChanged(const QString& layerKey, bool checked);
        void onLogScaleChanged(const QString& layerKey, bool checked);
        void updateStatisticsDisplay(const QString& layerKey,
                                     const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void updateControlsState();
        void updateLayerVisibility();
        LayerInspectState& getOrCreateLayerState(const QString& layerKey);
        QColor getLayerColor(const QString& layerKey) const;
        void onLayerSliderChanged(const QString& layerKey, int minValue, int maxValue);
        QString currentLayerCameraId() const;

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QHash<QString, LayerInfoGroup> m_layerInfoGroups;
        QHash<QString, LayerInspectState> m_layerStates;
        QStringList m_availableLayerKeys;
        QStringList m_availableCameraIds;
        QVBoxLayout* m_histogramContainerLayout{nullptr};
        QPushButton* m_drawMeasurementLineButton{nullptr};
        QPushButton* m_clearMeasurementLinesButton{nullptr};
        QLabel* m_measurementInfoLabel{nullptr};
        InspectCrossSectionWidget* m_crossSectionWidget{nullptr};
        QGroupBox* m_crossSectionGroup{nullptr};
        QPushButton* m_drawCrossSectionButton{nullptr};
        QPushButton* m_clearCrossSectionButton{nullptr};
        bool m_cameraInitialized{false};
        QString m_currentLayerKey;
        QString m_measurementLayerKey;
        QString m_crossSectionLayerKey;
    };
}
