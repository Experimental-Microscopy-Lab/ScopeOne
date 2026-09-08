#pragma once

#include "scopeone/SignalSource.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace scopeone::plugins
{
    class SimulatedPmtSource final : public scopeone::core::SignalSource
    {
    public:
        explicit SimulatedPmtSource(QObject* parent = nullptr);
        ~SimulatedPmtSource() override;

        bool start(const scopeone::core::SignalAcquisitionConfig& config,
                   QString* errorMessage = nullptr) override;
        void stop() override;
        scopeone::core::SignalSourceState state() const override;
        QString stateMessage() const override;

    private:
        struct Settings
        {
            double voltage{1.0};
            double gain{1.0};
            double baseRate{1000000.0};
            double darkCountRate{500.0};
            double modulationFrequency{2.0};
            double scanFrameRate{2.0};
            QString waveform{QStringLiteral("constant")};
            QString noiseMode{QStringLiteral("poisson")};
            QString pattern{QStringLiteral("beads")};
        };

        void run(std::stop_token stopToken,
                 const scopeone::core::SignalAcquisitionConfig& config,
                 const Settings& settings);
        void runStream(std::stop_token stopToken,
                       const scopeone::core::SignalAcquisitionConfig& config,
                       const Settings& settings);
        void runScan(std::stop_token stopToken,
                     const scopeone::core::SignalAcquisitionConfig& config,
                     const Settings& settings);
        double rateAt(double timeSeconds, const Settings& settings) const;
        void setState(scopeone::core::SignalSourceState state, const QString& message);

        mutable std::mutex m_stateMutex;
        std::mutex m_threadMutex;
        std::jthread m_worker;
        std::atomic<scopeone::core::SignalSourceState> m_state{
            scopeone::core::SignalSourceState::Idle};
        QString m_stateMessage{QStringLiteral("Simulated PMT is idle")};
    };
}
