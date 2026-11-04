#pragma once

#include "scopeone/SignalSource.h"

#include <QColor>
#include <QElapsedTimer>
#include <QRectF>
#include <QVector>
#include <QWidget>

#include <array>
#include <limits>

namespace scopeone::ui
{
    enum class ScopeTriggerMode
    {
        Roll,
        Auto,
        Normal
    };

    enum class ScopeTriggerSlope
    {
        Rising,
        Falling
    };

    struct ScopeChannel
    {
        bool enabled{false};
        QString sourceId;
        QString unit;
        double scale{1.0};
        double offset{0.0};
        bool acCoupled{false};
    };

    // An oscilloscope screen for the signal sources: ten by eight divisions, up to four channels
    // on one time axis, a software edge trigger on the received samples, cursors and the usual
    // measurements. The display can be held while the sources keep acquiring.
    class OscilloscopeView final : public QWidget
    {
        Q_OBJECT

    public:
        static constexpr int kChannelCount = 4;
        static constexpr int kHorizontalDivisions = 10;
        static constexpr int kVerticalDivisions = 8;

        explicit OscilloscopeView(QWidget* parent = nullptr);

        void setChannel(int index, const ScopeChannel& channel);
        void setTimebase(double secondsPerDivision);
        void setTrigger(ScopeTriggerMode mode,
                        int channel,
                        ScopeTriggerSlope slope,
                        double level,
                        double position);
        void setCursors(bool visible, int channel);
        void setRunning(bool running);
        bool isRunning() const;
        void armSingle();
        void appendChunk(const scopeone::core::TimeSeriesChunk& chunk);
        void clear();
        bool displayedRange(int channel, double* minimum, double* maximum) const;

        static QColor channelColor(int index);
        static QString formatValue(double value, const QString& unit);

    signals:
        void runningChanged(bool running);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;

    private:
        enum class DisplayState
        {
            Roll,
            Triggered,
            Auto,
            Ready
        };

        // The received samples of one channel. Old samples are dropped by moving the start, and
        // the vectors are compacted only now and then, so trimming does not copy on every chunk.
        struct Trace
        {
            QVector<double> times;
            QVector<double> values;
            qsizetype begin{0};

            bool empty() const;
            double first() const;
            double last() const;
            qsizetype indexAtOrAfter(double time) const;
            void append(const scopeone::core::TimeSeriesChunk& chunk);
            void trim(double cutoff, qsizetype maximumSamples);
            void clear();
        };

        struct Measurement
        {
            bool valid{false};
            double minimum{0.0};
            double maximum{0.0};
            double mean{0.0};
            double rms{0.0};
            double frequency{std::numeric_limits<double>::quiet_NaN()};
        };

        struct Frame
        {
            bool valid{false};
            double start{0.0};
            double end{0.0};
            double triggerTime{std::numeric_limits<double>::quiet_NaN()};
            std::array<QVector<double>, kChannelCount> times;
            std::array<QVector<double>, kChannelCount> values;
            std::array<Measurement, kChannelCount> measurements;
        };

        double screenSeconds() const;
        double newestTime() const;
        bool triggerShown() const;
        void refresh();
        void processTrigger();
        void buildFrame(double start, double end, double triggerTime);
        void resetTriggerSearch();
        double divisionsOf(int channel, double value) const;
        void drawGrid(QPainter& painter) const;
        void drawTraces(QPainter& painter) const;
        void drawMarkers(QPainter& painter) const;
        void drawCursors(QPainter& painter) const;
        void drawReadouts(QPainter& painter, double top) const;
        QPointF cursorPoint(int cursor) const;

        std::array<ScopeChannel, kChannelCount> m_channels;
        std::array<Trace, kChannelCount> m_traces;
        Frame m_frame;
        double m_timebase{1e-3};
        ScopeTriggerMode m_mode{ScopeTriggerMode::Auto};
        int m_triggerChannel{0};
        ScopeTriggerSlope m_slope{ScopeTriggerSlope::Rising};
        double m_triggerLevel{0.0};
        double m_triggerPosition{0.5};
        bool m_running{true};
        bool m_singleArmed{false};
        bool m_dirty{false};
        bool m_triggerArmed{false};
        double m_scanTime{-std::numeric_limits<double>::infinity()};
        QElapsedTimer m_sinceTrigger;
        DisplayState m_state{DisplayState::Ready};
        bool m_cursorsVisible{false};
        int m_cursorChannel{0};
        // Two time cursors as fractions of the screen width, then two level cursors in divisions
        std::array<double, 4> m_cursors{0.3, 0.7, 2.0, -2.0};
        int m_dragCursor{-1};
        QRectF m_screen;
    };
}
