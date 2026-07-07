#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QHash>
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
            QString cameraId;
            bool processed{false};
            scopeone::core::ScopeOneCore::HistogramStats stats;
            bool hasStats{false};
            bool autoStretchEnabled{false};
            bool displayRangeValid{false};
            int displayMin{0};
            int displayMax{255};
            int maxDisplayValue{255};
        };

        explicit InspectWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        ~InspectWidget() override = default;

        void onCameraInitialized(bool initialized);
        void setCurrentLayer(const QString& layerKey);
        void setAvailableLayers(const QStringList& layerKeys);
        void setAvailableCameras(const QStringList& cameraIds);
        void setCrossSectionVisible(bool visible);
        void setLayerInspect(const QString& layerKey,
                             const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void clearInspect();
        void clearCrossSectionProfile();
        void setLayerCrossSectionProfile(const QString& layerKey, const QVector<int>& values);

    signals:
        void displayRangeChanged(const QString& layerKey,
                                 int minLevel,
                                 int maxLevel,
                                 int maxDisplayValue);
        void requestDrawCrossSectionLayer(const QString& layerKey);
        void requestClearCrossSection();

    private:
        struct LayerInfoGroup
        {
            QString layerKey;
            QString cameraId;
            bool processed{false};
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
        void applyAutoStretch(LayerInspectState& state);
        LayerInspectState& getOrCreateLayerState(const QString& layerKey, const QString& cameraId, bool processed);
        QColor getLayerColor(const QString& layerKey) const;
        void onLayerSliderChanged(const QString& layerKey, int minValue, int maxValue);
        QString currentLayerCameraId() const;

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        QHash<QString, LayerInfoGroup> m_layerInfoGroups;
        QHash<QString, LayerInspectState> m_layerStates;
        QStringList m_availableLayerKeys;
        QStringList m_availableCameraIds;
        QVBoxLayout* m_histogramContainerLayout{nullptr};
        InspectCrossSectionWidget* m_crossSectionWidget{nullptr};
        QGroupBox* m_crossSectionGroup{nullptr};
        QPushButton* m_drawCrossSectionButton{nullptr};
        QPushButton* m_clearCrossSectionButton{nullptr};
        bool m_cameraInitialized{false};
        QString m_currentLayerKey;
    };
}
