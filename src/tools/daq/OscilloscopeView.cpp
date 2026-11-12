#include "OscilloscopeView.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace scopeone::ui
{
    namespace
    {
        const QColor kBackground(16, 20, 24);
        const QColor kGridColor(58, 68, 78);
        const QColor kBorderColor(96, 108, 120);
        const QColor kTextColor(210, 216, 222);
        const QColor kTriggerColor(255, 159, 28);
        const QColor kCursorColor(255, 159, 28);
        const QColor kStoppedColor(235, 80, 70);
        const QColor kRunningColor(95, 227, 95);

        // The raw samples kept per channel, a second at 1 MHz
        constexpr qsizetype kMaximumRawSamples = 1 << 20;
        // A screen is cut into this many buckets, about one per pixel column of a wide screen
        constexpr int kBucketsPerScreen = 2048;
        // Above this many samples on the screen a channel is drawn and measured from its buckets
        constexpr qsizetype kRawFrameSamples = 16384;
        // The newest raw samples the frequency is counted on, so it stays exact on a long timebase
        constexpr qsizetype kFrequencySamples = 65536;
        constexpr int kRefreshMilliseconds = 33;
        constexpr qint64 kAutoTriggerMilliseconds = 200;
        constexpr qint64 kFrequencyMilliseconds = 250;
        // The newest raw samples of the trigger channel its noise is measured on, a few times a second
        constexpr qsizetype kNoiseSamples = 4096;
        constexpr qint64 kHysteresisMilliseconds = 250;
        // How far from a marker the mouse still takes it
        constexpr double kReach = 9.0;
        // A triggered screen whose part after the edge takes longer than this is drawn as it fills
        constexpr double kProgressiveSeconds = 0.1;
    }

    bool OscilloscopeView::Trace::empty() const
    {
        return begin >= times.size();
    }

    double OscilloscopeView::Trace::first() const
    {
        return times[begin];
    }

    double OscilloscopeView::Trace::last() const
    {
        return times.constLast();
    }

    qsizetype OscilloscopeView::Trace::indexAtOrAfter(double time) const
    {
        return std::lower_bound(times.cbegin() + begin, times.cend(), time) - times.cbegin();
    }

    double OscilloscopeView::Trace::earliest(double bucketSeconds) const
    {
        double time = empty() ? std::numeric_limits<double>::infinity() : first();
        if (!buckets.empty())
        {
            time = std::min(time, static_cast<double>(buckets.front().index) * bucketSeconds);
        }
        return time;
    }

    void OscilloscopeView::Trace::append(const scopeone::core::TimeSeriesChunk& chunk, double bucketSeconds)
    {
        // No reserve here: reserving the exact new size on every chunk made each chunk reallocate
        // and copy the whole history, which froze the screen on a long timebase
        const qsizetype count = chunk.values.size();
        interval = chunk.sampleIntervalSeconds;
        for (qsizetype index = 0; index < count; ++index)
        {
            const double time = chunk.startTimeSeconds + static_cast<double>(index) * chunk.sampleIntervalSeconds;
            const double value = chunk.values[index];
            times.append(time);
            values.append(value);
            addToBucket(time, value, bucketSeconds);
        }
    }

    // Samples arrive in time order, so a sample never belongs to a bucket before the last one
    void OscilloscopeView::Trace::addToBucket(double time, double value, double bucketSeconds)
    {
        const auto index = static_cast<qint64>(std::floor(time / bucketSeconds));
        if (buckets.empty() || index > buckets.back().index)
        {
            buckets.push_back({index, value, value, value, value * value, 1});
            return;
        }
        Bucket& bucket = buckets.back();
        bucket.minimum = std::min(bucket.minimum, value);
        bucket.maximum = std::max(bucket.maximum, value);
        bucket.sum += value;
        bucket.squares += value * value;
        ++bucket.count;
    }

    void OscilloscopeView::Trace::trim(double cutoff, qsizetype maximumSamples, qint64 retainedBuckets)
    {
        if (!buckets.empty())
        {
            const qint64 oldest = buckets.back().index - retainedBuckets;
            while (buckets.front().index < oldest)
            {
                buckets.pop_front();
            }
        }
        if (empty())
        {
            return;
        }
        begin = std::max(indexAtOrAfter(cutoff), times.size() - maximumSamples);
        if (begin > 65536 && begin > times.size() / 2)
        {
            times.remove(0, begin);
            values.remove(0, begin);
            begin = 0;
        }
    }

    // A new timebase cuts the buckets anew from the raw samples, so the part of the history that
    // only the old buckets held is gone
    void OscilloscopeView::Trace::rebuildBuckets(double bucketSeconds)
    {
        buckets.clear();
        for (qsizetype index = begin; index < times.size(); ++index)
        {
            addToBucket(times[index], values[index], bucketSeconds);
        }
    }

    void OscilloscopeView::Trace::clear()
    {
        times.clear();
        values.clear();
        buckets.clear();
        begin = 0;
    }

    OscilloscopeView::OscilloscopeView(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(480, 320);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &OscilloscopeView::refresh);
        timer->start(kRefreshMilliseconds);
        m_sinceTrigger.start();
        m_sinceFrequency.start();
        m_sinceHysteresis.start();
        m_frequencies.fill(std::numeric_limits<double>::quiet_NaN());
    }

    void OscilloscopeView::setChannel(int index, const ScopeChannel& channel)
    {
        if (m_channels[index].sourceId != channel.sourceId)
        {
            m_traces[index].clear();
            m_frame.times[index].clear();
            m_frame.minima[index].clear();
            m_frame.maxima[index].clear();
            m_frame.measurements[index] = {};
            m_frequencies[index] = std::numeric_limits<double>::quiet_NaN();
        }
        m_channels[index] = channel;
        m_dirty = true;
        update();
    }

    void OscilloscopeView::setTimebase(double secondsPerDivision)
    {
        if (m_timebase == secondsPerDivision)
        {
            return;
        }
        m_timebase = secondsPerDivision;
        for (Trace& trace : m_traces)
        {
            trace.rebuildBuckets(bucketSeconds());
        }
        m_dirty = true;
        update();
    }

    void OscilloscopeView::setTrigger(ScopeTriggerMode mode,
                                      int channel,
                                      ScopeTriggerSlope slope,
                                      double level,
                                      double position)
    {
        if (mode != m_mode || channel != m_triggerChannel || slope != m_slope)
        {
            m_triggerArmed = false;
            m_pendingTrigger = std::numeric_limits<double>::quiet_NaN();
            m_triggerHysteresis = std::numeric_limits<double>::quiet_NaN();
        }
        m_mode = mode;
        m_triggerChannel = channel;
        m_slope = slope;
        m_triggerLevel = level;
        m_triggerPosition = position;
        m_dirty = true;
        update();
    }

    void OscilloscopeView::setCursors(bool visible, int channel)
    {
        m_cursorsVisible = visible;
        m_cursorChannel = channel;
        update();
    }

    void OscilloscopeView::setRunning(bool running)
    {
        if (!running)
        {
            m_singleArmed = false;
        }
        if (m_running == running)
        {
            return;
        }
        m_running = running;
        m_dirty = true;
        update();
        emit runningChanged(running);
    }

    bool OscilloscopeView::isRunning() const
    {
        return m_running;
    }

    bool OscilloscopeView::isSingleArmed() const
    {
        return m_singleArmed;
    }

    void OscilloscopeView::armSingle()
    {
        // Only a trigger after the button counts, as on a scope
        resetTriggerSearch();
        const double newest = newestTime();
        if (std::isfinite(newest))
        {
            m_scanTime = newest;
        }
        m_singleArmed = true;
        m_state = DisplayState::Ready;
        if (!m_running)
        {
            m_running = true;
            emit runningChanged(true);
        }
        m_dirty = true;
        update();
    }

    void OscilloscopeView::appendChunk(const scopeone::core::TimeSeriesChunk& chunk)
    {
        const double retained = 2.0 * screenSeconds() + 0.1;
        for (int index = 0; index < kChannelCount; ++index)
        {
            if (m_channels[index].sourceId != chunk.sourceId)
            {
                continue;
            }
            Trace& trace = m_traces[index];
            // A source that starts again counts its time from zero
            if (!trace.empty() && chunk.startTimeSeconds < trace.last())
            {
                trace.clear();
                if (index == m_triggerChannel)
                {
                    resetTriggerSearch();
                }
            }
            trace.append(chunk, bucketSeconds());
            trace.trim(trace.last() - retained, kMaximumRawSamples, 2 * kBucketsPerScreen + 1);
            m_dirty = true;
        }
    }

    void OscilloscopeView::clear()
    {
        for (Trace& trace : m_traces)
        {
            trace.clear();
        }
        m_frame = {};
        m_frequencies.fill(std::numeric_limits<double>::quiet_NaN());
        m_singleArmed = false;
        m_state = m_mode == ScopeTriggerMode::Roll ? DisplayState::Roll : DisplayState::Ready;
        resetTriggerSearch();
        update();
    }

    bool OscilloscopeView::displayedRange(int channel, double* minimum, double* maximum) const
    {
        const Measurement& measurement = m_frame.measurements[channel];
        if (!m_frame.valid || !measurement.valid)
        {
            return false;
        }
        *minimum = measurement.minimum;
        *maximum = measurement.maximum;
        return true;
    }

    double OscilloscopeView::displayedFrequency(int channel) const
    {
        const Measurement& measurement = m_frame.measurements[channel];
        return m_frame.valid && measurement.valid ? measurement.frequency : std::numeric_limits<double>::quiet_NaN();
    }

    void OscilloscopeView::setMessage(const QString& message)
    {
        m_message = message;
        update();
    }

    QColor OscilloscopeView::channelColor(int index)
    {
        static const std::array<QColor, kChannelCount> colors{
            QColor(245, 211, 63), QColor(57, 208, 240), QColor(240, 79, 208), QColor(95, 227, 95)};
        return colors[static_cast<std::size_t>(index)];
    }

    QString OscilloscopeView::formatValue(double value, const QString& unit)
    {
        if (!std::isfinite(value))
        {
            return QStringLiteral("-");
        }
        if (value == 0.0)
        {
            return QStringLiteral("0 %1").arg(unit);
        }
        static const char* const prefixes[] = {"p", "n", "µ", "m", "", "k", "M", "G"};
        const int exponent = std::clamp(
            static_cast<int>(std::floor(std::log10(std::abs(value)) / 3.0)) * 3, -12, 9);
        const double mantissa = value / std::pow(10.0, exponent);
        return QStringLiteral("%1 %2%3")
            .arg(QString::number(mantissa, 'g', 4),
                 QString::fromUtf8(prefixes[(exponent + 12) / 3]),
                 unit);
    }

    double OscilloscopeView::screenSeconds() const
    {
        return kHorizontalDivisions * m_timebase;
    }

    double OscilloscopeView::bucketSeconds() const
    {
        return screenSeconds() / kBucketsPerScreen;
    }

    // The newest time every shown channel has reached, so a frame ends where all of them have data
    double OscilloscopeView::newestTime() const
    {
        double newest = std::numeric_limits<double>::infinity();
        bool any = false;
        for (int index = 0; index < kChannelCount; ++index)
        {
            if (m_channels[index].enabled && !m_traces[index].empty())
            {
                newest = std::min(newest, m_traces[index].last());
                any = true;
            }
        }
        return any ? newest : std::numeric_limits<double>::quiet_NaN();
    }

    bool OscilloscopeView::triggerShown() const
    {
        return m_mode != ScopeTriggerMode::Roll || m_singleArmed;
    }

    void OscilloscopeView::refresh()
    {
        // A hidden or minimized screen waits; the samples keep arriving and it catches up when shown
        if (!m_dirty || !m_running || !isVisible() || window()->isMinimized())
        {
            return;
        }
        m_dirty = false;
        if (triggerShown())
        {
            processTrigger();
        }
        else
        {
            const double newest = newestTime();
            if (std::isfinite(newest))
            {
                buildFrame(newest - screenSeconds(), newest, std::numeric_limits<double>::quiet_NaN());
            }
            m_state = DisplayState::Roll;
        }
        update();
    }

    // Scans the trigger channel once over the samples that arrived, from where the last scan
    // ended. The trigger re-arms only after the signal moves the hysteresis past the level, so
    // noise on a slow edge does not trigger twice. An edge whose screen has been received is
    // shown; an edge still waiting for the part after it is kept and, on a slow timebase, drawn
    // as the samples come in, as a scope does, and edges are not looked for until it is complete.
    void OscilloscopeView::processTrigger()
    {
        const double screen = screenSeconds();
        const double before = m_triggerPosition * screen;
        const double after = screen - before;
        const double newest = newestTime();
        if (!std::isfinite(newest))
        {
            return;
        }

        double complete = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(m_pendingTrigger) && m_pendingTrigger + after <= newest)
        {
            complete = m_pendingTrigger;
            m_pendingTrigger = std::numeric_limits<double>::quiet_NaN();
        }
        const Trace& trace = m_traces[m_triggerChannel];
        if (m_channels[m_triggerChannel].enabled && !trace.empty())
        {
            if (!std::isfinite(m_triggerHysteresis) || m_sinceHysteresis.elapsed() >= kHysteresisMilliseconds)
            {
                updateHysteresis();
            }
            const double hysteresis = m_triggerHysteresis;
            const bool rising = m_slope == ScopeTriggerSlope::Rising;
            // The time before the edge may be held only by the buckets
            qsizetype index = trace.indexAtOrAfter(
                std::max(m_scanTime, trace.earliest(bucketSeconds()) + before));
            if (index < trace.times.size() && trace.times[index] == m_scanTime)
            {
                ++index;
            }
            for (; index < trace.times.size() && trace.times[index] <= newest; ++index)
            {
                const double value = trace.values[index];
                const bool beyond = rising ? value >= m_triggerLevel : value <= m_triggerLevel;
                const bool rearmed = rising ? value < m_triggerLevel - hysteresis
                                            : value > m_triggerLevel + hysteresis;
                if (rearmed)
                {
                    m_triggerArmed = true;
                }
                else if (m_triggerArmed && beyond && index > trace.begin)
                {
                    const double previousValue = trace.values[index - 1];
                    const double previousTime = trace.times[index - 1];
                    const double fraction = (m_triggerLevel - previousValue) / (value - previousValue);
                    const double crossing = previousTime + std::clamp(fraction, 0.0, 1.0)
                        * (trace.times[index] - previousTime);
                    m_triggerArmed = false;
                    if (crossing + after <= newest)
                    {
                        complete = crossing;
                    }
                    else if (!std::isfinite(m_pendingTrigger))
                    {
                        m_pendingTrigger = crossing;
                    }
                }
                m_scanTime = trace.times[index];
            }
        }

        if (std::isfinite(complete))
        {
            buildFrame(complete - before, complete + after, complete);
            m_state = DisplayState::Triggered;
            m_sinceTrigger.restart();
            if (m_singleArmed)
            {
                m_pendingTrigger = std::numeric_limits<double>::quiet_NaN();
                setRunning(false);
            }
            return;
        }
        if (std::isfinite(m_pendingTrigger) && after > kProgressiveSeconds)
        {
            buildFrame(m_pendingTrigger - before, m_pendingTrigger + after, m_pendingTrigger);
            m_state = DisplayState::Triggered;
            m_sinceTrigger.restart();
            return;
        }
        // Without an edge Auto runs free on every refresh, so a slow screen still moves smoothly
        if (m_mode == ScopeTriggerMode::Auto && !m_singleArmed && !std::isfinite(m_pendingTrigger)
            && m_sinceTrigger.elapsed() > kAutoTriggerMilliseconds)
        {
            buildFrame(newest - screen, newest, std::numeric_limits<double>::quiet_NaN());
            m_state = DisplayState::Auto;
            return;
        }
        if (m_state != DisplayState::Triggered || m_singleArmed)
        {
            m_state = DisplayState::Ready;
        }
    }

    // Takes the screen from the raw samples when they reach back over it and are few enough to
    // draw one by one, and from the buckets otherwise, so a frame holds at most a few thousand
    // points per channel whatever the sample rate and the timebase
    void OscilloscopeView::buildFrame(double start, double end, double triggerTime)
    {
        m_frame.valid = true;
        m_frame.start = start;
        m_frame.end = end;
        m_frame.triggerTime = triggerTime;
        const double width = bucketSeconds();
        const bool updateFrequency = m_singleArmed || m_sinceFrequency.elapsed() >= kFrequencyMilliseconds;
        if (updateFrequency)
        {
            m_sinceFrequency.restart();
        }
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            QVector<double>& times = m_frame.times[channel];
            QVector<double>& minima = m_frame.minima[channel];
            QVector<double>& maxima = m_frame.maxima[channel];
            Measurement& measurement = m_frame.measurements[channel];
            times.clear();
            minima.clear();
            maxima.clear();
            measurement = {};
            const Trace& trace = m_traces[channel];
            if (!m_channels[channel].enabled || (trace.empty() && trace.buckets.empty()))
            {
                continue;
            }

            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            double sum = 0.0;
            double squares = 0.0;
            double count = 0.0;
            qsizetype first = 0;
            qsizetype last = 0;
            if (!trace.empty())
            {
                first = trace.indexAtOrAfter(start);
                last = std::upper_bound(trace.times.cbegin() + first, trace.times.cend(), end)
                    - trace.times.cbegin();
            }
            const bool raw = !trace.empty()
                && trace.first() <= std::max(start, trace.earliest(width))
                && last - first <= kRawFrameSamples;
            m_frame.bucketed[channel] = !raw;
            if (raw)
            {
                times = trace.times.mid(first, last - first);
                minima = trace.values.mid(first, last - first);
                maxima = minima;
                for (const double value : minima)
                {
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                    sum += value;
                    squares += value * value;
                }
                count = static_cast<double>(minima.size());
            }
            else
            {
                const auto firstIndex = static_cast<qint64>(std::floor(start / width));
                const auto lastIndex = static_cast<qint64>(std::floor(end / width));
                auto bucket = std::lower_bound(trace.buckets.cbegin(), trace.buckets.cend(), firstIndex,
                                               [](const Bucket& item, qint64 index) { return item.index < index; });
                for (; bucket != trace.buckets.cend() && bucket->index <= lastIndex; ++bucket)
                {
                    times.append((static_cast<double>(bucket->index) + 0.5) * width);
                    minima.append(bucket->minimum);
                    maxima.append(bucket->maximum);
                    minimum = std::min(minimum, bucket->minimum);
                    maximum = std::max(maximum, bucket->maximum);
                    sum += bucket->sum;
                    squares += bucket->squares;
                    count += static_cast<double>(bucket->count);
                }
            }
            if (count <= 0.0)
            {
                continue;
            }
            measurement.valid = true;
            measurement.minimum = minimum;
            measurement.maximum = maximum;
            measurement.mean = sum / count;
            measurement.rms = std::sqrt(squares / count);
            // The frequency is the one costly measurement and a number read by eye, so it follows a
            // few times a second; a held screen always gets its own
            measurement.frequency = updateFrequency
                                        ? frequencyOf(channel, start, end, measurement)
                                        : m_frequencies[channel];
            m_frequencies[channel] = measurement.frequency;
        }
    }

    // Counts rising crossings of the mean, re-armed a tenth of the peak to peak below it. The
    // newest raw samples on the screen give the exact frequency of a fast signal; a signal too
    // slow for them to hold three crossings is counted on the means of the buckets.
    double OscilloscopeView::frequencyOf(int channel,
                                         double start,
                                         double end,
                                         const Measurement& measurement) const
    {
        const double hysteresis = 0.1 * (measurement.maximum - measurement.minimum);
        if (hysteresis <= 0.0)
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        struct Counter
        {
            double mean;
            double hysteresis;
            bool armed{false};
            int crossings{0};
            double first{0.0};
            double last{0.0};

            void add(double time, double value)
            {
                if (value < mean - hysteresis)
                {
                    armed = true;
                }
                else if (armed && value >= mean)
                {
                    armed = false;
                    if (crossings == 0)
                    {
                        first = time;
                    }
                    last = time;
                    ++crossings;
                }
            }

            double frequency() const
            {
                return last > first ? (crossings - 1) / (last - first) : std::numeric_limits<double>::quiet_NaN();
            }
        };

        const Trace& trace = m_traces[channel];
        if (!trace.empty())
        {
            const qsizetype last = std::upper_bound(trace.times.cbegin() + trace.begin, trace.times.cend(), end)
                - trace.times.cbegin();
            const qsizetype first = std::max(trace.indexAtOrAfter(start), last - kFrequencySamples);
            Counter counter{measurement.mean, hysteresis};
            for (qsizetype index = first; index < last; ++index)
            {
                counter.add(trace.times[index], trace.values[index]);
            }
            if (counter.crossings >= 3 || !m_frame.bucketed[channel])
            {
                return counter.crossings >= 2 ? counter.frequency() : std::numeric_limits<double>::quiet_NaN();
            }
        }

        const double width = bucketSeconds();
        const auto firstIndex = static_cast<qint64>(std::floor(start / width));
        const auto lastIndex = static_cast<qint64>(std::floor(end / width));
        auto bucket = std::lower_bound(trace.buckets.cbegin(), trace.buckets.cend(), firstIndex,
                                       [](const Bucket& item, qint64 index) { return item.index < index; });
        Counter counter{measurement.mean, hysteresis};
        for (; bucket != trace.buckets.cend() && bucket->index <= lastIndex; ++bucket)
        {
            counter.add((static_cast<double>(bucket->index) + 0.5) * width,
                        bucket->sum / static_cast<double>(bucket->count));
        }
        return counter.crossings >= 2 ? counter.frequency() : std::numeric_limits<double>::quiet_NaN();
    }

    void OscilloscopeView::resetTriggerSearch()
    {
        m_triggerArmed = false;
        m_pendingTrigger = std::numeric_limits<double>::quiet_NaN();
        m_scanTime = -std::numeric_limits<double>::infinity();
        m_triggerHysteresis = std::numeric_limits<double>::quiet_NaN();
    }

    // A fixed tenth of a division let the noise on a slow edge trigger on the other slope, so the
    // picture jumped, and on a signal smaller than that never re-armed, so nothing was shown. The
    // hysteresis now is six standard deviations of the noise, at most 40 % of the peak to peak so
    // a real edge still re-arms. The noise is taken from the second difference, which cancels a
    // slow signal, by its median absolute deviation, which ignores the edges.
    void OscilloscopeView::updateHysteresis()
    {
        m_sinceHysteresis.restart();
        const Trace& trace = m_traces[m_triggerChannel];
        const double scale = m_channels[m_triggerChannel].scale;
        const qsizetype count = std::min(kNoiseSamples, trace.times.size() - trace.begin);
        if (count < 8)
        {
            m_triggerHysteresis = 0.1 * scale;
            return;
        }
        const qsizetype first = trace.times.size() - count;
        std::vector<double> differences(static_cast<std::size_t>(count - 2));
        double minimum = trace.values[first];
        double maximum = minimum;
        for (qsizetype index = 0; index < count - 2; ++index)
        {
            const double* value = trace.values.constData() + first + index;
            differences[static_cast<std::size_t>(index)] = value[2] - 2.0 * value[1] + value[0];
            minimum = std::min({minimum, value[1], value[2]});
            maximum = std::max({maximum, value[1], value[2]});
        }
        const auto middle = differences.begin() + static_cast<std::ptrdiff_t>(differences.size() / 2);
        std::nth_element(differences.begin(), middle, differences.end());
        const double median = *middle;
        for (double& difference : differences)
        {
            difference = std::abs(difference - median);
        }
        std::nth_element(differences.begin(), middle, differences.end());
        const double sigma = 1.4826 * *middle / std::sqrt(6.0);
        m_triggerHysteresis = std::min(std::max(6.0 * sigma, 0.02 * scale), 0.4 * (maximum - minimum));
    }

    double OscilloscopeView::divisionsAt(double y) const
    {
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        return std::clamp((m_screen.center().y() - y) / divisionHeight,
                          -kVerticalDivisions / 2.0, kVerticalDivisions / 2.0);
    }

    double OscilloscopeView::triggerY() const
    {
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        return std::clamp(m_screen.center().y() - divisionsOf(m_triggerChannel, m_triggerLevel) * divisionHeight,
                          m_screen.top(), m_screen.bottom());
    }

    // The level that zero, or with AC coupling the mean, is drawn at
    double OscilloscopeView::channelTagY(int channel) const
    {
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        return std::clamp(m_screen.center().y()
                              - m_channels[channel].offset / m_channels[channel].scale * divisionHeight,
                          m_screen.top(), m_screen.bottom());
    }

    // The trigger marker right of the screen, the trigger position above it, the channel markers
    // left of it and the cursors on it
    OscilloscopeView::Hit OscilloscopeView::hitTest(const QPointF& point) const
    {
        if (triggerShown())
        {
            if (point.x() >= m_screen.right() && point.x() <= m_screen.right() + 30.0
                && std::abs(point.y() - triggerY()) <= kReach)
            {
                return {Handle::TriggerLevel, m_triggerChannel};
            }
            const double x = m_screen.left() + m_triggerPosition * m_screen.width();
            if (point.y() <= m_screen.top() && point.y() >= m_screen.top() - 16.0 && std::abs(point.x() - x) <= kReach)
            {
                return {Handle::TriggerPosition, 0};
            }
        }
        if (point.x() <= m_screen.left() && point.x() >= m_screen.left() - 30.0)
        {
            Hit hit;
            double nearest = kReach;
            for (int channel = 0; channel < kChannelCount; ++channel)
            {
                const double distance = std::abs(point.y() - channelTagY(channel));
                if (m_channels[channel].enabled && distance <= nearest)
                {
                    nearest = distance;
                    hit = {Handle::ChannelOffset, channel};
                }
            }
            return hit;
        }
        if (m_cursorsVisible && m_screen.contains(point))
        {
            Hit hit;
            double nearest = kReach;
            for (int cursor = 0; cursor < 4; ++cursor)
            {
                const QPointF position = cursorPoint(cursor);
                const double distance = cursor < 2 ? std::abs(point.x() - position.x())
                                                   : std::abs(point.y() - position.y());
                if (distance < nearest)
                {
                    nearest = distance;
                    hit = {Handle::Cursor, cursor};
                }
            }
            return hit;
        }
        return {};
    }

    // The value of a channel in divisions above the centre line; with AC coupling the mean of
    // the frame is taken off, while the trigger and the measurements keep the raw value
    double OscilloscopeView::divisionsOf(int channel, double value) const
    {
        const ScopeChannel& settings = m_channels[channel];
        const double mean = settings.acCoupled && m_frame.measurements[channel].valid
                                ? m_frame.measurements[channel].mean
                                : 0.0;
        return (value - mean + settings.offset) / settings.scale;
    }

    void OscilloscopeView::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setFont(font());
        const QFontMetricsF metrics(font());
        const double lineHeight = metrics.height() + 2.0;

        int readoutLines = 1 + (m_cursorsVisible ? 1 : 0);
        for (const ScopeChannel& channel : m_channels)
        {
            readoutLines += channel.enabled ? 1 : 0;
        }
        m_screen = QRectF(rect()).adjusted(32.0, 18.0, -32.0, -(readoutLines * lineHeight + 12.0));
        if (m_screen.width() < 40.0 || m_screen.height() < 40.0)
        {
            painter.fillRect(rect(), kBackground);
            return;
        }

        // The background and the grid change only with the size, and the dotted grid is slow to
        // draw, so they are drawn once into a pixmap
        const qreal ratio = devicePixelRatioF();
        if (m_gridCache.size() != size() * ratio || m_gridScreen != m_screen)
        {
            m_gridCache = QPixmap(size() * ratio);
            m_gridCache.setDevicePixelRatio(ratio);
            m_gridCache.fill(kBackground);
            QPainter gridPainter(&m_gridCache);
            drawGrid(gridPainter);
            m_gridScreen = m_screen;
        }
        painter.drawPixmap(0, 0, m_gridCache);
        drawTraces(painter);
        drawMarkers(painter);
        if (m_cursorsVisible)
        {
            drawCursors(painter);
        }

        QString stateText;
        QColor stateColor = kRunningColor;
        if (!m_running)
        {
            stateText = tr("Stop");
            stateColor = kStoppedColor;
        }
        else
        {
            switch (m_state)
            {
            case DisplayState::Roll: stateText = tr("Roll"); break;
            case DisplayState::Triggered: stateText = tr("Trig'd"); break;
            case DisplayState::Auto: stateText = tr("Auto"); break;
            case DisplayState::Ready:
                stateText = m_singleArmed ? tr("Single") : tr("Ready");
                stateColor = kTriggerColor;
                break;
            }
        }
        painter.setPen(stateColor);
        const QRectF inside = m_screen.adjusted(6.0, 4.0, -6.0, -4.0);
        painter.drawText(inside, Qt::AlignTop | Qt::AlignRight, stateText);
        // The rate the samples really come at, which a DAQ device may hold below the one asked for
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            if (m_channels[channel].enabled && m_traces[channel].interval > 0.0)
            {
                painter.setPen(kTextColor);
                painter.drawText(inside, Qt::AlignTop | Qt::AlignLeft,
                                 formatValue(1.0 / m_traces[channel].interval, QStringLiteral("S/s")));
                break;
            }
        }

        if (!m_message.isEmpty())
        {
            painter.setPen(kStoppedColor);
            painter.drawText(inside, (m_frame.valid ? Qt::AlignBottom | Qt::AlignLeft : Qt::AlignCenter)
                                 | Qt::TextWordWrap, m_message);
        }
        else if (!m_frame.valid)
        {
            painter.setPen(kTextColor);
            painter.drawText(m_screen, Qt::AlignCenter, m_running ? tr("Waiting for samples") : tr("Press Run"));
        }
        drawReadouts(painter, m_screen.bottom() + 8.0);
    }

    void OscilloscopeView::drawGrid(QPainter& painter) const
    {
        const double divisionWidth = m_screen.width() / kHorizontalDivisions;
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        QPen gridPen(kGridColor, 1.0, Qt::DotLine);
        painter.setPen(gridPen);
        for (int division = 1; division < kHorizontalDivisions; ++division)
        {
            const double x = m_screen.left() + division * divisionWidth;
            painter.drawLine(QPointF(x, m_screen.top()), QPointF(x, m_screen.bottom()));
        }
        for (int division = 1; division < kVerticalDivisions; ++division)
        {
            const double y = m_screen.top() + division * divisionHeight;
            painter.drawLine(QPointF(m_screen.left(), y), QPointF(m_screen.right(), y));
        }

        // Ticks of a fifth of a division on the two centre lines
        painter.setPen(QPen(kGridColor, 1.0));
        const QPointF centre = m_screen.center();
        for (int tick = 1; tick < kHorizontalDivisions * 5; ++tick)
        {
            const double x = m_screen.left() + tick * divisionWidth / 5.0;
            painter.drawLine(QPointF(x, centre.y() - 3.0), QPointF(x, centre.y() + 3.0));
        }
        for (int tick = 1; tick < kVerticalDivisions * 5; ++tick)
        {
            const double y = m_screen.top() + tick * divisionHeight / 5.0;
            painter.drawLine(QPointF(centre.x() - 3.0, y), QPointF(centre.x() + 3.0, y));
        }
        painter.setPen(QPen(kBorderColor, 1.0));
        painter.drawRect(m_screen);
    }

    void OscilloscopeView::drawTraces(QPainter& painter) const
    {
        if (!m_frame.valid || m_frame.end <= m_frame.start)
        {
            return;
        }
        painter.save();
        painter.setClipRect(m_screen);
        const double span = m_frame.end - m_frame.start;
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        const double centreY = m_screen.center().y();
        const int columns = std::max(1, static_cast<int>(m_screen.width()));
        for (int channel = kChannelCount - 1; channel >= 0; --channel)
        {
            const QVector<double>& times = m_frame.times[channel];
            const QVector<double>& lows = m_frame.minima[channel];
            const QVector<double>& highs = m_frame.maxima[channel];
            if (!m_channels[channel].enabled || times.isEmpty())
            {
                continue;
            }
            const auto mapX = [&](double time)
            {
                return m_screen.left() + (time - m_frame.start) / span * m_screen.width();
            };
            const auto mapY = [&](double value)
            {
                return centreY - divisionsOf(channel, value) * divisionHeight;
            };
            if (!m_frame.bucketed[channel] && times.size() <= 2 * columns)
            {
                // Smoothing pays only while the samples are farther apart than the pixels
                const bool smooth = times.size() <= columns;
                painter.setRenderHint(QPainter::Antialiasing, smooth);
                painter.setPen(QPen(channelColor(channel), smooth ? 1.2 : 0.0));
                QVector<QPointF> points(times.size());
                for (qsizetype index = 0; index < times.size(); ++index)
                {
                    points[index] = QPointF(mapX(times[index]), mapY(lows[index]));
                }
                painter.drawPolyline(points.constData(), static_cast<int>(points.size()));
                painter.setRenderHint(QPainter::Antialiasing, false);
                continue;
            }
            // A cosmetic pen of one pixel is the fastest the raster engine draws
            painter.setPen(QPen(channelColor(channel), 0.0));

            // Keep the extremes of each pixel column when samples or buckets outnumber the pixels
            QVector<double> minima(columns, std::numeric_limits<double>::infinity());
            QVector<double> maxima(columns, -std::numeric_limits<double>::infinity());
            for (qsizetype index = 0; index < times.size(); ++index)
            {
                const int column = std::clamp(
                    static_cast<int>((times[index] - m_frame.start) / span * columns), 0, columns - 1);
                minima[column] = std::min(minima[column], lows[index]);
                maxima[column] = std::max(maxima[column], highs[index]);
            }
            QVector<QLineF> lines;
            lines.reserve(columns);
            double previousLow = std::numeric_limits<double>::quiet_NaN();
            double previousHigh = previousLow;
            for (int column = 0; column < columns; ++column)
            {
                if (!std::isfinite(minima[column]))
                {
                    previousLow = std::numeric_limits<double>::quiet_NaN();
                    continue;
                }
                const double x = m_screen.left() + column;
                double low = mapY(minima[column]);
                double high = mapY(maxima[column]);
                // Join each column to the last one so a steep edge has no gap
                if (std::isfinite(previousLow))
                {
                    low = std::max(low, previousHigh);
                    high = std::min(high, previousLow);
                }
                lines.append(QLineF(x, low, x, high));
                previousLow = mapY(minima[column]);
                previousHigh = mapY(maxima[column]);
            }
            painter.drawLines(lines);
        }
        painter.restore();
    }

    void OscilloscopeView::drawMarkers(QPainter& painter) const
    {
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            if (!m_channels[channel].enabled)
            {
                continue;
            }
            const double y = channelTagY(channel);
            const double right = m_screen.left() - 3.0;
            const QPolygonF tag({QPointF(right - 24.0, y - 7.0), QPointF(right - 6.0, y - 7.0),
                                 QPointF(right, y), QPointF(right - 6.0, y + 7.0),
                                 QPointF(right - 24.0, y + 7.0)});
            painter.setPen(Qt::NoPen);
            painter.setBrush(channelColor(channel));
            painter.drawPolygon(tag);
            painter.setPen(kBackground);
            painter.drawText(QRectF(right - 24.0, y - 7.0, 18.0, 14.0), Qt::AlignCenter,
                             QString::number(channel + 1));
        }

        if (triggerShown())
        {
            const double y = triggerY();
            const double left = m_screen.right() + 3.0;
            const QPolygonF tag({QPointF(left, y), QPointF(left + 6.0, y - 7.0),
                                 QPointF(left + 24.0, y - 7.0), QPointF(left + 24.0, y + 7.0),
                                 QPointF(left + 6.0, y + 7.0)});
            painter.setPen(Qt::NoPen);
            painter.setBrush(channelColor(m_triggerChannel));
            painter.drawPolygon(tag);
            painter.setPen(kBackground);
            painter.drawText(QRectF(left + 6.0, y - 7.0, 18.0, 14.0), Qt::AlignCenter, QStringLiteral("T"));

            const double x = m_screen.left() + m_triggerPosition * m_screen.width();
            const QPolygonF arrow({QPointF(x - 6.0, m_screen.top() - 12.0),
                                   QPointF(x + 6.0, m_screen.top() - 12.0),
                                   QPointF(x, m_screen.top() - 2.0)});
            painter.setBrush(kTriggerColor);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(arrow);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    QPointF OscilloscopeView::cursorPoint(int cursor) const
    {
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        if (cursor < 2)
        {
            return {m_screen.left() + m_cursors[cursor] * m_screen.width(), m_screen.top()};
        }
        return {m_screen.left(), m_screen.center().y() - m_cursors[cursor] * divisionHeight};
    }

    void OscilloscopeView::drawCursors(QPainter& painter) const
    {
        painter.setPen(QPen(kCursorColor, 1.0, Qt::DashLine));
        for (int cursor = 0; cursor < 4; ++cursor)
        {
            const QPointF point = cursorPoint(cursor);
            const QString label = cursor < 2 ? QStringLiteral("X%1").arg(cursor + 1)
                                             : QStringLiteral("Y%1").arg(cursor - 1);
            if (cursor < 2)
            {
                painter.drawLine(QPointF(point.x(), m_screen.top()), QPointF(point.x(), m_screen.bottom()));
                painter.drawText(QPointF(point.x() + 3.0, m_screen.bottom() - 4.0), label);
            }
            else
            {
                painter.drawLine(QPointF(m_screen.left(), point.y()), QPointF(m_screen.right(), point.y()));
                painter.drawText(QPointF(m_screen.left() + 4.0, point.y() - 3.0), label);
            }
        }
    }

    void OscilloscopeView::drawReadouts(QPainter& painter, double top) const
    {
        const QFontMetricsF metrics(font());
        const double lineHeight = metrics.height() + 2.0;
        const double spacing = metrics.horizontalAdvance(QStringLiteral("    "));
        double y = top + metrics.ascent();
        double x = m_screen.left();
        const auto write = [&](const QString& text, const QColor& color)
        {
            painter.setPen(color);
            painter.drawText(QPointF(x, y), text);
            x += metrics.horizontalAdvance(text) + spacing;
        };

        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            const ScopeChannel& settings = m_channels[channel];
            if (settings.enabled)
            {
                write(QStringLiteral("CH%1 %2/div%3")
                          .arg(channel + 1)
                          .arg(formatValue(settings.scale, settings.unit),
                               settings.acCoupled ? QStringLiteral(" AC") : QString()),
                      channelColor(channel));
            }
        }
        write(QStringLiteral("%1/div").arg(formatValue(m_timebase, QStringLiteral("s"))), kTextColor);
        if (triggerShown())
        {
            write(QStringLiteral("T CH%1 %2 %3")
                      .arg(m_triggerChannel + 1)
                      .arg(m_slope == ScopeTriggerSlope::Rising ? QStringLiteral("↑")
                                                                : QStringLiteral("↓"),
                           formatValue(m_triggerLevel, m_channels[m_triggerChannel].unit)),
                  kTriggerColor);
        }

        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            const ScopeChannel& settings = m_channels[channel];
            if (!settings.enabled)
            {
                continue;
            }
            y += lineHeight;
            x = m_screen.left();
            const Measurement& measurement = m_frame.measurements[channel];
            const QColor color = channelColor(channel);
            write(QStringLiteral("CH%1").arg(channel + 1), color);
            if (!measurement.valid)
            {
                write(tr("no data"), color);
                continue;
            }
            write(tr("Vpp %1").arg(formatValue(measurement.maximum - measurement.minimum, settings.unit)), color);
            write(tr("Min %1").arg(formatValue(measurement.minimum, settings.unit)), color);
            write(tr("Max %1").arg(formatValue(measurement.maximum, settings.unit)), color);
            write(tr("Mean %1").arg(formatValue(measurement.mean, settings.unit)), color);
            write(tr("RMS %1").arg(formatValue(measurement.rms, settings.unit)), color);
            write(tr("Freq %1").arg(formatValue(measurement.frequency, QStringLiteral("Hz"))), color);
        }

        if (m_cursorsVisible)
        {
            y += lineHeight;
            x = m_screen.left();
            const double span = m_frame.valid ? m_frame.end - m_frame.start : screenSeconds();
            const double deltaTime = std::abs(m_cursors[1] - m_cursors[0]) * span;
            const ScopeChannel& settings = m_channels[m_cursorChannel];
            // A level cursor reads the value the cursor channel has at that height
            const auto level = [&](double divisions)
            {
                return divisions * settings.scale - settings.offset;
            };
            write(QStringLiteral("Δt %1").arg(formatValue(deltaTime, QStringLiteral("s"))), kCursorColor);
            write(QStringLiteral("1/Δt %1")
                      .arg(formatValue(deltaTime > 0.0 ? 1.0 / deltaTime
                                                       : std::numeric_limits<double>::quiet_NaN(),
                                       QStringLiteral("Hz"))),
                  kCursorColor);
            write(QStringLiteral("CH%1 Y1 %2").arg(m_cursorChannel + 1)
                      .arg(formatValue(level(m_cursors[2]), settings.unit)), kCursorColor);
            write(QStringLiteral("Y2 %1").arg(formatValue(level(m_cursors[3]), settings.unit)), kCursorColor);
            write(QStringLiteral("ΔY %1")
                      .arg(formatValue(std::abs(m_cursors[2] - m_cursors[3]) * settings.scale, settings.unit)),
                  kCursorColor);
        }
    }

    void OscilloscopeView::mousePressEvent(QMouseEvent* event)
    {
        m_drag = event->button() == Qt::LeftButton ? hitTest(event->position()) : Hit{};
    }

    void OscilloscopeView::mouseMoveEvent(QMouseEvent* event)
    {
        const QPointF point = event->position();
        if (m_drag.handle == Handle::None)
        {
            const Hit hit = hitTest(point);
            const bool horizontal = hit.handle == Handle::TriggerPosition
                || (hit.handle == Handle::Cursor && hit.index < 2);
            if (hit.handle == Handle::None)
            {
                unsetCursor();
            }
            else
            {
                setCursor(horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            }
            return;
        }
        const double fraction = std::clamp((point.x() - m_screen.left()) / m_screen.width(), 0.0, 1.0);
        const double divisions = divisionsAt(point.y());
        switch (m_drag.handle)
        {
        case Handle::Cursor:
            m_cursors[m_drag.index] = m_drag.index < 2 ? fraction : divisions;
            break;
        case Handle::TriggerLevel:
        {
            const ScopeChannel& channel = m_channels[m_triggerChannel];
            const double mean = channel.acCoupled && m_frame.measurements[m_triggerChannel].valid
                                    ? m_frame.measurements[m_triggerChannel].mean
                                    : 0.0;
            m_triggerLevel = divisions * channel.scale - channel.offset + mean;
            m_dirty = true;
            emit triggerLevelChanged(m_triggerLevel);
            break;
        }
        case Handle::TriggerPosition:
            m_triggerPosition = fraction;
            m_dirty = true;
            emit triggerPositionChanged(fraction);
            break;
        case Handle::ChannelOffset:
            m_channels[m_drag.index].offset = divisions * m_channels[m_drag.index].scale;
            emit channelOffsetChanged(m_drag.index, m_channels[m_drag.index].offset);
            break;
        case Handle::None:
            break;
        }
        update();
    }

    void OscilloscopeView::mouseReleaseEvent(QMouseEvent*)
    {
        m_drag = {};
    }

    // Wheel up zooms in: over a channel marker to a smaller scale, elsewhere to a shorter
    // timebase; with Shift or over the trigger marker it moves the trigger level a tenth of a
    // division. A touchpad's small steps add up to whole notches.
    void OscilloscopeView::wheelEvent(QWheelEvent* event)
    {
        // Shift turns the wheel sideways on some systems
        const QPoint angle = event->angleDelta();
        m_wheelAngle += angle.y() != 0 ? angle.y() : angle.x();
        const int notches = m_wheelAngle / 120;
        event->accept();
        if (notches == 0)
        {
            return;
        }
        m_wheelAngle -= notches * 120;
        const Hit hit = hitTest(event->position());
        if ((event->modifiers() & Qt::ShiftModifier) || hit.handle == Handle::TriggerLevel)
        {
            m_triggerLevel += notches * 0.1 * m_channels[m_triggerChannel].scale;
            m_dirty = true;
            update();
            emit triggerLevelChanged(m_triggerLevel);
        }
        else if (hit.handle == Handle::ChannelOffset)
        {
            emit channelScaleStepRequested(hit.index, -notches);
        }
        else
        {
            emit timebaseStepRequested(-notches);
        }
    }
}
