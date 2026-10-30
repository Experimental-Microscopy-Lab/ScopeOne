#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace scopeone::ui
{
    class ScopeOneToolContext;

    class ParticleDetectionDialog : public QDialog
    {
        Q_OBJECT

    public:
        ParticleDetectionDialog(scopeone::core::ScopeOneCore* core,
                                ScopeOneToolContext& context,
                                QWidget* parent = nullptr);

        void reject() override;

    private:
        void setupUI();
        void refreshCameras();
        void ensurePreviewRunning(const QString& cameraId);
        void analyzeCurrentFrame();
        void handleDetectionFinished(
            quint64 requestId,
            const QString& layerKey,
            const scopeone::core::ScopeOneCore::ParticleDetectionResult& result,
            const QString& errorMessage);
        QString selectedCameraId() const;

        scopeone::core::ScopeOneCore* m_core{nullptr};
        ScopeOneToolContext& m_context;
        QComboBox* m_cameraCombo{nullptr};
        QSpinBox* m_thresholdSpinBox{nullptr};
        QSpinBox* m_minAreaSpinBox{nullptr};
        QSpinBox* m_maxAreaSpinBox{nullptr};
        QCheckBox* m_autoUpdateCheckBox{nullptr};
        QPushButton* m_analyzeButton{nullptr};
        QLabel* m_statusLabel{nullptr};
        QTimer* m_autoUpdateTimer{nullptr};
        QString m_requestedPreviewCameraId;
        quint64 m_activeDetectionRequestId{0};
    };
}
