#include "SimulatedPmtSource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <utility>

namespace scopeone::plugins
{
    using namespace scopeone::core;

    namespace
    {
        constexpr auto kSourceId = "pmt:simulator";
        constexpr double kTickPeriodSeconds = 1.0e-9;
        constexpr double Pi = 3.14159265358979323846;
        constexpr quint32 kPhotonCode = 1;

        int photonCount(double mean,
                        const QString& noiseMode,
                        std::mt19937_64& random)
        {
            if (noiseMode == QStringLiteral("none"))
            {
                return std::max(0, static_cast<int>(std::llround(mean)));
            }
            if (noiseMode == QStringLiteral("gaussian"))
            {
                const double sigma = std::sqrt(std::max(0.0, mean));
                return std::max(0, static_cast<int>(std::llround(
                    std::normal_distribution<double>(mean, sigma)(random))));
            }
            return std::poisson_distribution<int>(mean)(random);
        }

        double gaussianSpot(double x,
                            double y,
                            double centerX,
                            double centerY,
                            double sigma)
        {
            const double dx = x - centerX;
            const double dy = y - centerY;
            return std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
        }

        struct Bead
        {
            double x;
            double y;
            double brightness;
        };

        constexpr std::array<Bead, 32> Beads{{
            {0.08, 0.10, 0.91}, {0.19, 0.08, 0.84}, {0.32, 0.12, 0.98},
            {0.46, 0.07, 0.88}, {0.62, 0.11, 0.94}, {0.77, 0.08, 0.82},
            {0.91, 0.13, 0.96}, {0.13, 0.25, 0.87}, {0.29, 0.29, 1.00},
            {0.48, 0.23, 0.83}, {0.69, 0.27, 0.92}, {0.87, 0.31, 0.86},
            {0.08, 0.43, 0.95}, {0.24, 0.47, 0.81}, {0.42, 0.39, 0.90},
            {0.61, 0.45, 0.97}, {0.80, 0.42, 0.85}, {0.94, 0.50, 0.93},
            {0.14, 0.63, 0.89}, {0.34, 0.58, 0.99}, {0.53, 0.66, 0.84},
            {0.72, 0.61, 0.96}, {0.89, 0.68, 0.80}, {0.07, 0.82, 0.92},
            {0.22, 0.88, 0.86}, {0.39, 0.79, 0.95}, {0.58, 0.86, 0.82},
            {0.76, 0.81, 0.98}, {0.92, 0.89, 0.88}, {0.47, 0.94, 0.91},
            {0.30, 0.70, 0.85}, {0.67, 0.75, 0.94}
        }};

        struct BeadDoublet
        {
            double x;
            double y;
            double firstBrightness;
            double secondBrightness;
        };

        constexpr std::array<BeadDoublet, 4> BeadDoublets{{
            {0.183, 0.176, 1.00, 0.92},
            {0.543, 0.335, 0.88, 0.96},
            {0.373, 0.515, 0.98, 0.86},
            {0.713, 0.925, 0.91, 1.00}
        }};

        double patternIntensity(const QString& pattern,
                                int column,
                                int line,
                                int width,
                                int height)
        {
            const double x = (column + 0.5) / width;
            const double y = (line + 0.5) / height;
            if (pattern == QStringLiteral("beads"))
            {
                const double resolutionScale = std::min(width, height) / 256.0;
                const double fwhmPixels = 4.0 * resolutionScale;
                const double sigmaPixels = fwhmPixels / 2.354820045;
                double value = 0.002;
                for (const Bead& bead : Beads)
                {
                    value += bead.brightness
                        * gaussianSpot(column + 0.5,
                                       line + 0.5,
                                       bead.x * width,
                                       bead.y * height,
                                       sigmaPixels);
                }
                const double doubletSpacingPixels = 4.0 * resolutionScale;
                for (const BeadDoublet& doublet : BeadDoublets)
                {
                    const double centerX = doublet.x * width;
                    const double centerY = doublet.y * height;
                    value += doublet.firstBrightness
                        * gaussianSpot(column + 0.5,
                                       line + 0.5,
                                       centerX - doubletSpacingPixels * 0.5,
                                       centerY,
                                       sigmaPixels);
                    value += doublet.secondBrightness
                        * gaussianSpot(column + 0.5,
                                       line + 0.5,
                                       centerX + doubletSpacingPixels * 0.5,
                                       centerY,
                                       sigmaPixels);
                }
                return std::min(value, 1.0);
            }
            if (pattern == QStringLiteral("usaf"))
            {
                const double barX = std::abs(std::sin(2.0 * Pi * 18.0 * x));
                const double barY = std::abs(std::sin(2.0 * Pi * 12.0 * y));
                return (x > 0.15 && x < 0.85 && y > 0.2 && y < 0.8)
                           ? std::max(barX, barY)
                           : 0.02;
            }
            if (pattern == QStringLiteral("grid"))
            {
                const double dx = x - 0.5;
                const double dy = y - 0.5;
                const double radius = std::sqrt(dx * dx + dy * dy);
                const double rings = 0.5 + 0.5 * std::cos(2.0 * Pi * 18.0 * radius);
                const double lines = 0.5 + 0.5 * std::cos(2.0 * Pi * 8.0 * x)
                    * std::cos(2.0 * Pi * 8.0 * y);
                return std::max(rings, lines);
            }

            double value = 0.02;
            value = std::max(value, gaussianSpot(x, y, 0.28, 0.35, 0.07));
            value = std::max(value, gaussianSpot(x, y, 0.62, 0.32, 0.09));
            value = std::max(value, gaussianSpot(x, y, 0.45, 0.65, 0.10));
            value = std::max(value,
                             0.5 + 0.5 * std::sin(2.0 * Pi * (4.0 * x + 2.0 * y)));
            return value;
        }
    }

    SimulatedPmtSource::SimulatedPmtSource(QObject* parent)
        : SignalSource(parent)
    {
    }

    SimulatedPmtSource::~SimulatedPmtSource()
    {
        std::lock_guard lock(m_threadMutex);
        if (m_worker.joinable())
        {
            m_worker.request_stop();
            m_worker.join();
        }
    }

    bool SimulatedPmtSource::start(const SignalAcquisitionConfig& config,
                                   QString* errorMessage)
    {
        if (config.sourceId.trimmed() != QString::fromLatin1(kSourceId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown simulated PMT source");
            }
            return false;
        }

        std::lock_guard lock(m_threadMutex);
        if (m_state == SignalSourceState::Starting
            || m_state == SignalSourceState::Running
            || m_state == SignalSourceState::Stopping)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Simulated PMT is already active");
            }
            return false;
        }
        if (m_worker.joinable())
        {
            m_worker.join();
        }

        Settings settings;
        settings.voltage = config.sourceSettings
            .value(QStringLiteral("voltage"), settings.voltage).toDouble();
        settings.gain = config.sourceSettings
            .value(QStringLiteral("gain"), settings.gain).toDouble();
        settings.baseRate = config.sourceSettings
            .value(QStringLiteral("baseRate"), settings.baseRate).toDouble();
        settings.darkCountRate = config.sourceSettings
            .value(QStringLiteral("darkCountRate"), settings.darkCountRate).toDouble();
        settings.modulationFrequency = config.sourceSettings
            .value(QStringLiteral("modulationFrequency"), settings.modulationFrequency).toDouble();
        settings.scanFrameRate = config.sourceSettings
            .value(QStringLiteral("scanFrameRate"), settings.scanFrameRate).toDouble();
        settings.waveform = config.sourceSettings
            .value(QStringLiteral("waveform"), settings.waveform).toString();
        settings.noiseMode = config.sourceSettings
            .value(QStringLiteral("noiseMode"), settings.noiseMode).toString();
        settings.pattern = config.sourceSettings
            .value(QStringLiteral("pattern"), settings.pattern).toString();

        setState(SignalSourceState::Starting, QStringLiteral("Starting simulated PMT"));
        m_worker = std::jthread([this, config, settings](std::stop_token stopToken)
        {
            run(stopToken, config, settings);
        });
        return true;
    }

    void SimulatedPmtSource::stop()
    {
        std::lock_guard lock(m_threadMutex);
        if (m_state == SignalSourceState::Starting
            || m_state == SignalSourceState::Running)
        {
            setState(SignalSourceState::Stopping, QStringLiteral("Stopping simulated PMT"));
            m_worker.request_stop();
        }
    }

    SignalSourceState SimulatedPmtSource::state() const
    {
        return m_state.load();
    }

    QString SimulatedPmtSource::stateMessage() const
    {
        std::lock_guard lock(m_stateMutex);
        return m_stateMessage;
    }

    void SimulatedPmtSource::setState(SignalSourceState state, const QString& message)
    {
        {
            std::lock_guard lock(m_stateMutex);
            m_stateMessage = message;
            m_state.store(state);
        }
        emit stateChanged(state, message);
    }

    double SimulatedPmtSource::rateAt(double timeSeconds, const Settings& settings) const
    {
        const double gain = settings.gain * std::pow(settings.voltage, 3.0);
        const double signalRate = settings.baseRate * gain + settings.darkCountRate;
        const double phase = 2.0 * Pi * settings.modulationFrequency * timeSeconds;

        if (settings.waveform == QStringLiteral("sine"))
        {
            return signalRate * (0.15 + 0.85 * (0.5 + 0.5 * std::sin(phase)));
        }
        if (settings.waveform == QStringLiteral("square"))
        {
            return signalRate * (std::sin(phase) >= 0.0 ? 1.0 : 0.15);
        }
        return signalRate;
    }

    void SimulatedPmtSource::run(std::stop_token stopToken,
                                 const SignalAcquisitionConfig& config,
                                 const Settings& settings)
    {
        setState(SignalSourceState::Running, QStringLiteral("Simulated PMT is running"));
        if (config.scanImage.enabled)
        {
            runScan(stopToken, config, settings);
        }
        else
        {
            runStream(stopToken, config, settings);
        }

        if (stopToken.stop_requested())
        {
            setState(SignalSourceState::Idle, QStringLiteral("Simulated PMT stopped"));
        }
        else
        {
            setState(SignalSourceState::Idle, QStringLiteral("Simulated PMT finished"));
        }
    }

    void SimulatedPmtSource::runStream(std::stop_token stopToken,
                                       const SignalAcquisitionConfig& config,
                                       const Settings& settings)
    {
        constexpr int chunkSize = 64;
        const double sampleInterval = config.sampleIntervalSeconds;
        const auto startTime = std::chrono::steady_clock::now();
        std::mt19937_64 random(std::random_device{}());
        quint64 sampleIndex = 0;
        quint64 totalEvents = 0;

        while (!stopToken.stop_requested()
               && sampleIndex * sampleInterval * 1000.0 < config.durationMs)
        {
            TimeSeriesChunk chunk;
            chunk.sourceId = QString::fromLatin1(kSourceId);
            chunk.quantity = QStringLiteral("Photon count rate");
            chunk.unit = QStringLiteral("counts/s");
            chunk.startTimeSeconds = sampleIndex * sampleInterval;
            chunk.sampleIntervalSeconds = sampleInterval;
            chunk.values.reserve(chunkSize);

            TimestampedEventChunk events;
            if (config.publishTimestampedEvents)
            {
                events.sourceId = QString::fromLatin1(kSourceId);
                events.tickPeriodSeconds = sampleInterval;
            }

            for (int index = 0; index < chunkSize; ++index)
            {
                const double timeSeconds = sampleIndex * sampleInterval;
                const double rate = rateAt(timeSeconds, settings);
                const int count = photonCount(rate * sampleInterval,
                                              settings.noiseMode,
                                              random);
                const double value = settings.noiseMode == QStringLiteral("none")
                                          ? rate
                                          : count / sampleInterval;
                totalEvents += static_cast<quint64>(count);
                if (config.publishTimestampedEvents)
                {
                    for (int event = 0; event < count; ++event)
                    {
                        events.eventTicks.append(sampleIndex);
                        events.eventCodes.append(kPhotonCode);
                    }
                }
                chunk.values.append(value);
                ++sampleIndex;
            }
            chunk.totalInputEvents = totalEvents;
            chunk.totalMarkers = 0;
            emit timeSeriesReady(chunk);
            if (events.isValid())
            {
                emit timestampedEventsReady(events);
            }
            std::this_thread::sleep_until(
                startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(sampleIndex * sampleInterval)));
        }
    }

    void SimulatedPmtSource::runScan(std::stop_token stopToken,
                                     const SignalAcquisitionConfig& config,
                                     const Settings& settings)
    {
        const ScanImageConfig& scan = config.scanImage;
        const double frameDuration = 1.0 / settings.scanFrameRate;
        const double lineDuration = frameDuration / scan.height;
        const int samplesPerLine = scan.width * 2;
        const double sampleDuration = lineDuration / samplesPerLine;
        const quint64 lineTicks = std::max<quint64>(
            1,
            static_cast<quint64>(std::llround(lineDuration / kTickPeriodSeconds)));
        const quint64 frameTicks = lineTicks * static_cast<quint64>(scan.height);
        const double actualLineDuration = lineTicks * kTickPeriodSeconds;
        const double actualFrameDuration = frameTicks * kTickPeriodSeconds;
        const auto startTime = std::chrono::steady_clock::now();
        std::mt19937_64 random(std::random_device{}());
        EventCountBinner binner(QString::fromLatin1(kSourceId),
                                QStringLiteral("Photon count rate"),
                                QStringLiteral("counts/s"),
                                kTickPeriodSeconds,
                                config.sampleIntervalSeconds);
        quint64 frameIndex = 0;

        while (!stopToken.stop_requested()
               && frameIndex * actualFrameDuration * 1000.0 < config.durationMs)
        {
            const quint64 frameStart = frameIndex * frameTicks;
            TimestampedEventChunk events;
            events.sourceId = QString::fromLatin1(kSourceId);
            events.tickPeriodSeconds = kTickPeriodSeconds;
            events.markerTicks.append(frameStart);
            events.markerCodes.append(scan.frameStartMarker);
            binner.addMarker(frameStart, scan.frameStartMarker);

            const double gain = settings.gain * std::pow(settings.voltage, 3.0);
            for (int line = 0; line < scan.height; ++line)
            {
                const quint64 lineStart = frameStart + lineTicks * static_cast<quint64>(line);
                for (int sample = 0; sample < samplesPerLine; ++sample)
                {
                    const int column = sample < scan.width
                                           ? sample
                                           : samplesPerLine - 1 - sample;
                    const double rate = patternIntensity(settings.pattern,
                                                         column,
                                                         line,
                                                         scan.width,
                                                         scan.height)
                        * settings.baseRate * gain
                        + settings.darkCountRate;
                    const int count = photonCount(rate * sampleDuration,
                                                  settings.noiseMode,
                                                  random);
                    const quint64 pixelStart = lineStart
                        + static_cast<quint64>(
                            static_cast<double>(sample) * lineTicks / samplesPerLine);
                    const quint64 pixelEnd = lineStart
                        + static_cast<quint64>(
                            static_cast<double>(sample + 1) * lineTicks / samplesPerLine);
                    const quint64 pixelWidth = std::max<quint64>(
                        1,
                        pixelEnd > pixelStart ? pixelEnd - pixelStart : 1);
                    std::uniform_int_distribution<quint64> offset(0, pixelWidth - 1);
                    for (int event = 0; event < count; ++event)
                    {
                        const quint64 tick = pixelStart + offset(random);
                        events.eventTicks.append(tick);
                        events.eventCodes.append(kPhotonCode);
                        binner.addEvent(tick);
                    }
                }

                const quint64 lineEnd = lineStart + lineTicks;
                events.markerTicks.append(lineEnd);
                events.markerCodes.append(scan.lineMarker);
                binner.addMarker(lineEnd, scan.lineMarker);
                for (const TimeSeriesChunk& chunk : binner.takeReadyChunks())
                {
                    emit timeSeriesReady(chunk);
                }
            }

            if (scan.frameEndMarker != 0)
            {
                events.markerTicks.append(frameStart + frameTicks);
                events.markerCodes.append(scan.frameEndMarker);
                binner.addMarker(frameStart + frameTicks, scan.frameEndMarker);
            }
            binner.advanceToElapsedSeconds((frameIndex + 1) * actualFrameDuration);
            for (const TimeSeriesChunk& chunk : binner.takeCompletedChunks())
            {
                emit timeSeriesReady(chunk);
            }
            emit timestampedEventsReady(events);
            ++frameIndex;

            std::this_thread::sleep_until(
                startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(frameIndex * actualFrameDuration)));
        }
    }
}
