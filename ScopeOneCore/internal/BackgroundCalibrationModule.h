#pragma once

#include "internal/ProcessingModule.h"
#include <deque>
#include <vector>

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
    public:
        QString id() const override { return QStringLiteral("background_calibration"); }
        QString name() const override { return "Background Calibration"; }
        QVariantMap parameters() const override;
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        bool resetState() override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        void resetCalibration();
        void computeBackground();
        ProcessingResult processRunningMean(const ImageFrame& sourceFrame,
                                            const ImageFrame& workingFrame);

        int m_calibrationFrames{101};
        std::deque<ImageFrame> m_buffer;
        std::vector<qint64> m_runningSum;
        ImageFrame m_background;
        bool m_calibrated{false};
        BackgroundOperation m_operation{BackgroundOperation::Subtract};
        BackgroundMethod m_method{BackgroundMethod::Median};
        BackgroundMode m_mode{BackgroundMode::Snapshot};
    };
}
