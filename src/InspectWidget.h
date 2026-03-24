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

namespace scopeone::ui {

class InspectCrossSectionWidget;
class InspectHistogramWidget;

class InspectWidget : public QWidget
{
    Q_OBJECT

public:
    struct CameraDisplayState {
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
    void setCurrentTarget(const QString& target);
    void setAvailableCameras(const QStringList& cameraIds);
    void setCrossSectionVisible(bool visible);
    void setFrameInspect(const QString& cameraId,
                          const scopeone::core::ScopeOneCore::HistogramStats& stats);
    void clearInspect();
    void clearCrossSectionProfile();
    void setCrossSectionProfile(const QString& cameraId, bool processed, const QVector<int>& values);

signals:
    void displayRangeChanged(const QString& cameraId,
                             bool processed,
                             int minLevel,
                             int maxLevel,
                             int maxDisplayValue);
    void requestDrawCrossSection(const QString& cameraId);
    void requestClearCrossSection();

private:
    struct CameraInfoGroup {
        QString streamKey;
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
    QWidget* createCameraInfoGroup(const QString& cameraId, bool processed);
    QWidget* createStatisticsGroup(CameraInfoGroup& infoGroup);
    void addCameraInfo(const QString& cameraId, bool processed);
    void removeCameraInfo(const QString& streamKey);
    void updateCameraInspect(const QString& cameraId,
                              bool processed,
                              const scopeone::core::ScopeOneCore::HistogramStats& stats);
    void onImageHistogramReady(const QString& cameraId,
                               bool processed,
                               const scopeone::core::ScopeOneCore::HistogramStats& stats);
    void onAutoButtonClicked(const QString& streamKey);
    void onFullButtonClicked(const QString& streamKey);
    void onAutoStretchChanged(const QString& streamKey, bool checked);
    void onLogScaleChanged(const QString& streamKey, bool checked);
    void updateStatisticsDisplay(const QString& cameraId,
                                 bool processed,
                                 const scopeone::core::ScopeOneCore::HistogramStats& stats);
    void updateControlsState();
    void applyAutoStretch(CameraDisplayState& state, const QString& streamKey);
    CameraDisplayState& getOrCreateCameraState(const QString& cameraId, bool processed);
    QColor getCameraColor(const QString& cameraId) const;
    void onCameraSliderChanged(const QString& streamKey, int minValue, int maxValue);
    bool isAllTarget(const QString& target) const;

    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    QHash<QString, CameraInfoGroup> m_cameraInfoGroups;
    QHash<QString, CameraDisplayState> m_cameraStates;
    QVBoxLayout* m_histogramContainerLayout{nullptr};
    InspectCrossSectionWidget* m_crossSectionWidget{nullptr};
    QGroupBox* m_crossSectionGroup{nullptr};
    QPushButton* m_drawCrossSectionButton{nullptr};
    QPushButton* m_clearCrossSectionButton{nullptr};
    bool m_cameraInitialized{false};
    QString m_currentTarget{QStringLiteral("All")};
};

}
