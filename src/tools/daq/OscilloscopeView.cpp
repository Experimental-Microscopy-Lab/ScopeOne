#include "OscilloscopeView.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>

#include <algorithm>
#include <cmath>

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

        // About two thousand screens of a fast timebase or a few minutes of a slow one per channel
        constexpr qsizetype kMaximumSamples = 4'000'000;
        constexpr int kRefreshMilliseconds = 33;
        constexpr qint64 kAutoTriggerMilliseconds = 200;
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

    void OscilloscopeView::Trace::append(const scopeone::core::TimeSeriesChunk& chunk)
    {
        const qsizetype count = chunk.values.size();
        times.reserve(times.size() + count);
        values.reserve(values.size() + count);
        for (qsizetype index = 0; index < count; ++index)
        {
            times.append(chunk.startTimeSeconds + static_cast<double>(index) * chunk.sampleIntervalSeconds);
            values.append(chunk.values[index]);
        }
    }

    void OscilloscopeView::Trace::trim(double cutoff, qsizetype maximumSamples)
    {
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

    void OscilloscopeView::Trace::clear()
    {
        times.clear();
        values.clear();
        begin = 0;
    }

    OscilloscopeView::OscilloscopeView(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(480, 320);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &OscilloscopeView::refresh);
        timer->start(kRefreshMilliseconds);
        m_sinceTrigger.start();
    }

    void OscilloscopeView::setChannel(int index, const ScopeChannel& channel)
    {
        if (m_channels[index].sourceId != channel.sourceId)
        {
            m_traces[index].clear();
            m_frame.times[index].clear();
            m_frame.values[index].clear();
            m_frame.measurements[index] = {};
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
            trace.append(chunk);
            trace.trim(trace.last() - retained, kMaximumSamples);
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
        if (!m_dirty || !m_running)
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
    // ended, and shows the latest edge whose whole screen has been received. The trigger re-arms
    // only after the signal moves a tenth of a division past the level, so noise on a slow edge
    // does not trigger twice.
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

        double crossing = std::numeric_limits<double>::quiet_NaN();
        const Trace& trace = m_traces[m_triggerChannel];
        if (m_channels[m_triggerChannel].enabled && !trace.empty())
        {
            const double limit = newest - after;
            const double hysteresis = 0.1 * m_channels[m_triggerChannel].scale;
            const bool rising = m_slope == ScopeTriggerSlope::Rising;
            qsizetype index = trace.indexAtOrAfter(std::max(m_scanTime, trace.first() + before));
            if (index < trace.times.size() && trace.times[index] == m_scanTime)
            {
                ++index;
            }
            for (; index < trace.times.size() && trace.times[index] <= limit; ++index)
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
                    crossing = previousTime + std::clamp(fraction, 0.0, 1.0)
                        * (trace.times[index] - previousTime);
                    m_triggerArmed = false;
                }
                m_scanTime = trace.times[index];
            }
        }

        if (std::isfinite(crossing))
        {
            buildFrame(crossing - before, crossing + after, crossing);
            m_state = DisplayState::Triggered;
            m_sinceTrigger.restart();
            if (m_singleArmed)
            {
                setRunning(false);
            }
            return;
        }
        if (m_mode == ScopeTriggerMode::Auto && !m_singleArmed
            && m_sinceTrigger.elapsed() > kAutoTriggerMilliseconds)
        {
            buildFrame(newest - screen, newest, std::numeric_limits<double>::quiet_NaN());
            m_state = DisplayState::Auto;
            m_sinceTrigger.restart();
            return;
        }
        if (m_state != DisplayState::Triggered || m_singleArmed)
        {
            m_state = DisplayState::Ready;
        }
    }

    void OscilloscopeView::buildFrame(double start, double end, double triggerTime)
    {
        m_frame.valid = true;
        m_frame.start = start;
        m_frame.end = end;
        m_frame.triggerTime = triggerTime;
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            QVector<double>& times = m_frame.times[channel];
            QVector<double>& values = m_frame.values[channel];
            Measurement& measurement = m_frame.measurements[channel];
            times.clear();
            values.clear();
            measurement = {};
            const Trace& trace = m_traces[channel];
            if (!m_channels[channel].enabled || trace.empty())
            {
                continue;
            }
            const qsizetype first = trace.indexAtOrAfter(start);
            const qsizetype last = std::upper_bound(trace.times.cbegin() + first, trace.times.cend(), end)
                - trace.times.cbegin();
            if (first >= last)
            {
                continue;
            }
            times = trace.times.mid(first, last - first);
            values = trace.values.mid(first, last - first);

            double minimum = values.constFirst();
            double maximum = minimum;
            double sum = 0.0;
            double squares = 0.0;
            for (const double value : values)
            {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
                sum += value;
                squares += value * value;
            }
            const double count = static_cast<double>(values.size());
            measurement.valid = true;
            measurement.minimum = minimum;
            measurement.maximum = maximum;
            measurement.mean = sum / count;
            measurement.rms = std::sqrt(squares / count);

            // Rising crossings of the mean, re-armed a tenth of the peak to peak below it
            const double hysteresis = 0.1 * (maximum - minimum);
            bool armed = false;
            int crossings = 0;
            double firstCrossing = 0.0;
            double lastCrossing = 0.0;
            for (qsizetype index = 0; index < values.size() && hysteresis > 0.0; ++index)
            {
                if (values[index] < measurement.mean - hysteresis)
                {
                    armed = true;
                }
                else if (armed && values[index] >= measurement.mean)
                {
                    armed = false;
                    if (crossings == 0)
                    {
                        firstCrossing = times[index];
                    }
                    lastCrossing = times[index];
                    ++crossings;
                }
            }
            if (crossings >= 2 && lastCrossing > firstCrossing)
            {
                measurement.frequency = (crossings - 1) / (lastCrossing - firstCrossing);
            }
        }
    }

    void OscilloscopeView::resetTriggerSearch()
    {
        m_triggerArmed = false;
        m_scanTime = -std::numeric_limits<double>::infinity();
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
        painter.fillRect(rect(), kBackground);
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
            return;
        }

        drawGrid(painter);
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
        painter.drawText(m_screen.adjusted(6.0, 4.0, -6.0, -4.0), Qt::AlignTop | Qt::AlignRight, stateText);

        if (!m_frame.valid)
        {
            painter.setPen(kTextColor);
            painter.drawText(m_screen, Qt::AlignCenter, tr("No signal data"));
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
            const QVector<double>& values = m_frame.values[channel];
            if (!m_channels[channel].enabled || values.isEmpty())
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
            painter.setPen(QPen(channelColor(channel), 1.2));
            if (values.size() <= 2 * columns)
            {
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.moveTo(mapX(times.constFirst()), mapY(values.constFirst()));
                for (qsizetype index = 1; index < values.size(); ++index)
                {
                    path.lineTo(mapX(times[index]), mapY(values[index]));
                }
                painter.drawPath(path);
                painter.setRenderHint(QPainter::Antialiasing, false);
                continue;
            }

            // Keep the extremes of each pixel column when samples outnumber the pixels
            QVector<double> minima(columns, std::numeric_limits<double>::infinity());
            QVector<double> maxima(columns, -std::numeric_limits<double>::infinity());
            for (qsizetype index = 0; index < values.size(); ++index)
            {
                const int column = std::clamp(
                    static_cast<int>((times[index] - m_frame.start) / span * columns), 0, columns - 1);
                minima[column] = std::min(minima[column], values[index]);
                maxima[column] = std::max(maxima[column], values[index]);
            }
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
                painter.drawLine(QPointF(x, low), QPointF(x, high));
                previousLow = mapY(minima[column]);
                previousHigh = mapY(maxima[column]);
            }
        }
        painter.restore();
    }

    void OscilloscopeView::drawMarkers(QPainter& painter) const
    {
        const double divisionHeight = m_screen.height() / kVerticalDivisions;
        const double centreY = m_screen.center().y();
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int channel = 0; channel < kChannelCount; ++channel)
        {
            if (!m_channels[channel].enabled)
            {
                continue;
            }
            // The level that zero, or with AC coupling the mean, is drawn at
            const double y = std::clamp(centreY - m_channels[channel].offset / m_channels[channel].scale
                                                    * divisionHeight,
                                        m_screen.top(), m_screen.bottom());
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
            const double level = centreY - divisionsOf(m_triggerChannel, m_triggerLevel) * divisionHeight;
            const double y = std::clamp(level, m_screen.top(), m_screen.bottom());
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
        m_dragCursor = -1;
        if (!m_cursorsVisible || !m_screen.contains(event->position()))
        {
            return;
        }
        double nearest = 8.0;
        for (int cursor = 0; cursor < 4; ++cursor)
        {
            const QPointF point = cursorPoint(cursor);
            const double distance = cursor < 2 ? std::abs(event->position().x() - point.x())
                                               : std::abs(event->position().y() - point.y());
            if (distance < nearest)
            {
                nearest = distance;
                m_dragCursor = cursor;
            }
        }
    }

    void OscilloscopeView::mouseMoveEvent(QMouseEvent* event)
    {
        if (m_dragCursor < 0)
        {
            return;
        }
        if (m_dragCursor < 2)
        {
            m_cursors[m_dragCursor] = std::clamp(
                (event->position().x() - m_screen.left()) / m_screen.width(), 0.0, 1.0);
        }
        else
        {
            const double divisionHeight = m_screen.height() / kVerticalDivisions;
            m_cursors[m_dragCursor] = std::clamp(
                (m_screen.center().y() - event->position().y()) / divisionHeight,
                -kVerticalDivisions / 2.0, kVerticalDivisions / 2.0);
        }
        update();
    }

    void OscilloscopeView::mouseReleaseEvent(QMouseEvent*)
    {
        m_dragCursor = -1;
    }
}
