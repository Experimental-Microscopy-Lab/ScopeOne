#pragma once

#include "internal/ProcessingModule.h"

#include <QHash>
#include <QMutex>

#include <deque>

namespace scopeone::core::internal
{
    enum class BackgroundMethod
    {
        Median,
        Mean,
        Maximum,
        Minimum
    };

    enum class BackgroundOperation
    {
        Subtract,
        Add,
        Multiply,
        Divide
    };

    enum class BackgroundMode
    {
        Snapshot,
        Running
    };

    class BackgroundCalibrationModule : public ProcessingModule
    {
        Q_OBJECT

    public:
        explicit BackgroundCalibrationModule(QObject* parent = nullptr);

        bool process(const ModuleInput& in, ModuleOutput& out) override;
        QString getModuleName() const override { return "Background Calibration"; }

        QVariantMap getParameters() const override;
        void setParameters(const QVariantMap& params) override;

        void resetCalibration();

    private:
        struct CameraState
        {
            std::deque<ImageFrame> buffer;
            ImageFrame background;
            bool calibrated{false};
        };

        ImageFrame computeBackground(const std::deque<ImageFrame>& buffer) const;

        int m_calibrationFrames;
        BackgroundOperation m_operation;
        BackgroundMethod m_method;
        BackgroundMode m_mode{BackgroundMode::Snapshot};
        QHash<QString, CameraState> m_states;
        QMutex m_mutex;
    };
}
