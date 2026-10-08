#pragma once

#include "scopeone/ImageSceneModel.h"
#include "scopeone/ScopeOneCore.h"

#include <QFutureWatcher>
#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QGroupBox;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace scopeone::ui
{
    class ImageWorkspace;
    class InspectCrossSectionWidget;

    class InspectWidget : public QWidget
    {
        Q_OBJECT

    public:
        // Pixel statistics of the selected rectangle or line
        struct SelectionStatistics
        {
            qint64 pixelCount{0};
            double mean{0.0};
            double stdDev{0.0};
            int minValue{0};
            int maxValue{0};
        };

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
        void refreshActiveViewer();
        void refreshSelection();

    private:
        struct LayerInfoGroup
        {
            QString layerKey;
            QGroupBox* groupBox{nullptr};
            QLabel* meanLabel{nullptr};
            QLabel* minLabel{nullptr};
            QLabel* maxLabel{nullptr};
            QLabel* stdDevLabel{nullptr};
            QLabel* pixelCountLabel{nullptr};
        };

        struct ViewerInspectState
        {
            QHash<QString, LayerInspectState> layerStates;
        };

        void setupUI();
        QWidget* createLayerInfoGroup(const QString& layerKey);
        QWidget* createStatisticsGroup(LayerInfoGroup& infoGroup);
        void addLayerInfo(const QString& layerKey);
        void removeLayerInfo(const QString& layerKey);
        void updateLayerInspect(const QString& layerKey,
                                const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void updateStatisticsDisplay(const QString& layerKey,
                                     const scopeone::core::ScopeOneCore::HistogramStats& stats);
        void updateControlsState();
        void updateLayerVisibility();
        void saveViewerState();
        void restoreViewerState();
        QString currentLayerKey() const;
        LayerInspectState& getOrCreateLayerState(const QString& layerKey);
        QString currentLayerCameraId() const;
        bool selectedMarkup(scopeone::core::ImageSceneModel::Markup& outMarkup) const;
        QStringList selectionSummary(const scopeone::core::ImageSceneModel::Markup& markup) const;
        void scheduleSelectionRefresh();
        void requestSelectionStats(const scopeone::core::ImageSceneModel::Markup& markup);
        void applySelectionStats();

        scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        scopeone::core::ImageSceneModel* m_sceneModel{nullptr};
        QWidget* m_contentContainer{nullptr};
        QVBoxLayout* m_contentLayout{nullptr};
        QHash<QString, LayerInfoGroup> m_layerInfoGroups;
        QHash<QString, LayerInspectState> m_layerStates;
        QStringList m_availableLayerKeys;
        QStringList m_availableCameraIds;
        QPushButton* m_rectangleToolButton{nullptr};
        QPushButton* m_lineToolButton{nullptr};
        QPushButton* m_deleteSelectionButton{nullptr};
        QLabel* m_selectionInfoLabel{nullptr};
        QWidget* m_selectionStatsWidget{nullptr};
        QLabel* m_selectionMeanLabel{nullptr};
        QLabel* m_selectionStdDevLabel{nullptr};
        QLabel* m_selectionMinLabel{nullptr};
        QLabel* m_selectionMaxLabel{nullptr};
        InspectCrossSectionWidget* m_crossSectionWidget{nullptr};
        QTimer* m_selectionRefreshTimer{nullptr};
        QFutureWatcher<SelectionStatistics>* m_selectionStatsWatcher{nullptr};
        QString m_selectionStatsMarkupId;
        bool m_selectionStatsPending{false};
        bool m_cameraInitialized{false};
        QHash<QString, ViewerInspectState> m_viewerStates;
        QString m_activeViewerStateId;
    };
}
