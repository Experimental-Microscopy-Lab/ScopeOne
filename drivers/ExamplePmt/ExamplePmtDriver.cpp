// A driver for a PMT, written the way a detector maker would write one. It uses only the
// headers of scopeone::DriverSDK. Where a real driver reads its counter or digitizer, this
// one draws photon counts on a thread of its own: a stream of count rates, or the photon events
// of a raster scan with its line and frame markers, from which ScopeOne assembles the image.
#include "scopeone/DriverSDK.h"

#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <thread>
#include <utility>

namespace example_pmt
{
    using namespace scopeone::core;

    namespace
    {
        constexpr auto kSourceId = "example.pmt";
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

        scopeone::core::SignalParameterDescriptor realParameter(
            const QString& key,
            const QString& name,
            double value,
            double minimum,
            double maximum,
            const QString& suffix)
        {
            scopeone::core::SignalParameterDescriptor parameter;
            parameter.key = key;
            parameter.name = name;
            parameter.type = scopeone::core::SignalParameterType::Real;
            parameter.defaultValue = value;
            parameter.hasRange = true;
            parameter.minimum = minimum;
            parameter.maximum = maximum;
            parameter.suffix = suffix;
            return parameter;
        }

        scopeone::core::SignalParameterDescriptor choiceParameter(
            const QString& key,
            const QString& name,
            const QStringList& values,
            const QStringList& names,
            const QString& value)
        {
            scopeone::core::SignalParameterDescriptor parameter;
            parameter.key = key;
            parameter.name = name;
            parameter.type = scopeone::core::SignalParameterType::Choice;
            parameter.defaultValue = value;
            for (int index = 0; index < values.size(); ++index)
            {
                parameter.choices.append(values[index]);
                parameter.choiceNames.append(names[index]);
            }
            return parameter;
        }

        scopeone::core::SignalSourceDescriptor sourceDescriptor()
        {
                        SignalSourceDescriptor descriptor;
            descriptor.id = QString::fromLatin1(kSourceId);
            descriptor.name = QStringLiteral("Example PMT");
            descriptor.provider = QStringLiteral("Example PMT");
            descriptor.quantity = QStringLiteral("Photon count rate");
            descriptor.unit = QStringLiteral("counts/s");
            descriptor.streamType = SignalStreamType::TimestampedEvents;
            descriptor.parameters.append(realParameter(
                QStringLiteral("voltage"),
                QStringLiteral("Control voltage"),
                1.0,
                0.0,
                1.25,
                QStringLiteral(" V")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("gain"),
                QStringLiteral("Gain"),
                1.0,
                1.0,
                100.0,
                QString()));
            descriptor.parameters.append(realParameter(
                QStringLiteral("baseRate"),
                QStringLiteral("Base rate"),
                1000000.0,
                10000.0,
                1000000.0,
                QStringLiteral(" counts/s")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("darkCountRate"),
                QStringLiteral("Dark count rate"),
                500.0,
                0.0,
                5000.0,
                QStringLiteral(" counts/s")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("modulationFrequency"),
                QStringLiteral("Modulation frequency"),
                2.0,
                0.0,
                1000.0,
                QStringLiteral(" Hz")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("scanFrameRate"),
                QStringLiteral("Scan frame rate"),
                2.0,
                0.5,
                10.0,
                QStringLiteral(" Hz")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("waveform"),
                QStringLiteral("Waveform"),
                {QStringLiteral("constant"), QStringLiteral("sine"), QStringLiteral("square")},
                {QStringLiteral("Constant"), QStringLiteral("Sine"), QStringLiteral("Square")},
                QStringLiteral("constant")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("noiseMode"),
                QStringLiteral("Noise"),
                {QStringLiteral("poisson"), QStringLiteral("gaussian"), QStringLiteral("none")},
                {QStringLiteral("Poisson"), QStringLiteral("Gaussian"), QStringLiteral("None")},
                QStringLiteral("poisson")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("pattern"),
                QStringLiteral("Pattern"),
                {QStringLiteral("beads"), QStringLiteral("usaf"),
                 QStringLiteral("grid"), QStringLiteral("cells")},
                {QStringLiteral("Fluorescent Beads"), QStringLiteral("USAF Target"),
                 QStringLiteral("Concentric Grid"), QStringLiteral("Cell Structure")},
                QStringLiteral("beads")));
            return descriptor;
        }
    }

    // The PMT and the counter behind it. It reports its chunks and states through the sinks of the provider.
    class PmtSource
    {
    public:
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

        ~PmtSource()
        {
            stop();
            if (m_worker.joinable())
            {
                m_worker.join();
            }
        }

        void setSinks(SignalSource::TimeSeriesSink timeSeries,
                      SignalSource::TimestampedEventSink timestampedEvents,
                      SignalSource::StateSink state)
        {
            QMutexLocker locker(&m_sinkMutex);
            m_timeSeriesSink = std::move(timeSeries);
            m_timestampedEventSink = std::move(timestampedEvents);
            m_stateSink = std::move(state);
        }

        bool start(const SignalAcquisitionConfig& config, QString* errorMessage)
        {
            if (m_running.load())
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("The PMT is already acquiring");
                }
                return false;
            }
            if (m_worker.joinable())
            {
                m_worker.join();
            }
            Settings settings;
            const QVariantMap& values = config.sourceSettings;
            settings.voltage = values.value(QStringLiteral("voltage"), settings.voltage).toDouble();
            settings.gain = values.value(QStringLiteral("gain"), settings.gain).toDouble();
            settings.baseRate = values.value(QStringLiteral("baseRate"), settings.baseRate).toDouble();
            settings.darkCountRate = values.value(QStringLiteral("darkCountRate"), settings.darkCountRate).toDouble();
            settings.modulationFrequency =
                values.value(QStringLiteral("modulationFrequency"), settings.modulationFrequency).toDouble();
            settings.scanFrameRate = values.value(QStringLiteral("scanFrameRate"), settings.scanFrameRate).toDouble();
            settings.waveform = values.value(QStringLiteral("waveform"), settings.waveform).toString();
            settings.noiseMode = values.value(QStringLiteral("noiseMode"), settings.noiseMode).toString();
            settings.pattern = values.value(QStringLiteral("pattern"), settings.pattern).toString();

            m_running.store(true);
            reportState(SignalSourceState::Running, QStringLiteral("The PMT is acquiring"));
            m_worker = std::jthread([this, config, settings](std::stop_token stopToken)
            {
                if (config.scanImage.enabled)
                {
                    runScan(stopToken, config, settings);
                }
                else
                {
                    runStream(stopToken, config, settings);
                }
                m_running.store(false);
                reportState(SignalSourceState::Idle, stopToken.stop_requested()
                                                         ? QStringLiteral("The PMT stopped")
                                                         : QStringLiteral("The PMT finished"));
            });
            return true;
        }

        void stop()
        {
            m_worker.request_stop();
        }

    private:
        double rateAt(double timeSeconds, const Settings& settings) const;
        void runStream(std::stop_token stopToken, const SignalAcquisitionConfig& config, const Settings& settings);
        void runScan(std::stop_token stopToken, const SignalAcquisitionConfig& config, const Settings& settings);

        void publish(const TimeSeriesChunk& chunk)
        {
            QMutexLocker locker(&m_sinkMutex);
            if (m_timeSeriesSink)
            {
                m_timeSeriesSink(chunk);
            }
        }

        void publish(const TimestampedEventChunk& chunk)
        {
            QMutexLocker locker(&m_sinkMutex);
            if (m_timestampedEventSink)
            {
                m_timestampedEventSink(chunk);
            }
        }

        void reportState(SignalSourceState state, const QString& message)
        {
            QMutexLocker locker(&m_sinkMutex);
            if (m_stateSink)
            {
                m_stateSink(QString::fromLatin1(kSourceId), state, message);
            }
        }

        QMutex m_sinkMutex;
        SignalSource::TimeSeriesSink m_timeSeriesSink;
        SignalSource::TimestampedEventSink m_timestampedEventSink;
        SignalSource::StateSink m_stateSink;
        std::atomic_bool m_running{false};
        std::jthread m_worker;
    };

    double PmtSource::rateAt(double timeSeconds, const Settings& settings) const
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

    void PmtSource::runStream(std::stop_token stopToken,
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
            publish(chunk);
            if (events.isValid())
            {
                publish(events);
            }
            std::this_thread::sleep_until(
                startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(sampleIndex * sampleInterval)));
        }
    }

    void PmtSource::runScan(std::stop_token stopToken,
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
                    publish(chunk);
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
                publish(chunk);
            }
            publish(events);
            ++frameIndex;

            std::this_thread::sleep_until(
                startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(frameIndex * actualFrameDuration)));
        }
    }

    class PmtDevice final : public Device, public SignalSource
    {
    public:
        HardwareProviderDescriptor descriptor() const override
        {
            return {QString::fromLatin1(kSourceId), QStringLiteral("Example PMT"), QStringLiteral("1.0.0")};
        }

        QList<SignalSourceDescriptor> signalSources() const override
        {
            return {sourceDescriptor()};
        }

        void setSignalSinks(TimeSeriesSink timeSeries,
                            TimestampedEventSink timestampedEvents,
                            StateSink state) override
        {
            m_pmt.setSinks(std::move(timeSeries), std::move(timestampedEvents), std::move(state));
        }

        bool startSignal(const SignalAcquisitionConfig& config, QString* errorMessage) override
        {
            if (config.sourceId != QString::fromLatin1(kSourceId))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unknown signal source %1").arg(config.sourceId);
                }
                return false;
            }
            return m_pmt.start(config, errorMessage);
        }

        void stopSignal(const QString& sourceId) override
        {
            if (sourceId == QString::fromLatin1(kSourceId))
            {
                m_pmt.stop();
            }
        }

    private:
        PmtSource m_pmt;
    };

    class ExamplePmtDriver final : public QObject, public Driver
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
        Q_INTERFACES(scopeone::core::Driver)

    public:
        QString providerId() const override
        {
            return QString::fromLatin1(kSourceId);
        }

        QStringList deviceNames() const override
        {
            return {QStringLiteral("Example PMT")};
        }

        DevicePtr createDevice(const QString& name, const QVariantMap&, QString* errorMessage) override
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            if (name != deviceNames().first())
            {
                if (errorMessage) *errorMessage = QStringLiteral("Unknown device %1").arg(name);
                return {};
            }
            return std::make_shared<PmtDevice>();
        }
    };
}

#include "ExamplePmtDriver.moc"
