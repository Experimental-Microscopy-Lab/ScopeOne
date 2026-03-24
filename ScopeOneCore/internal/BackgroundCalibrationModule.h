#pragma once

#include "internal/ProcessingModule.h"
#include <deque>

namespace scopeone::core::internal {

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
    void computeBackground();

    int m_calibrationFrames;
    std::deque<ImageFrame> m_buffer;
    ImageFrame m_background;
    bool m_calibrated;
    BackgroundOperation m_operation;
    BackgroundMethod m_method;
    BackgroundMode m_mode{BackgroundMode::Snapshot};
};

}
