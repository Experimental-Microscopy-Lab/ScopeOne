#pragma once

#include "scopeone/SignalSource.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QRectF>
#include <QVector>
#include <QWidget>

#include <array>
#include <deque>
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
        bool isSingleArmed() const;
        void armSingle();
        void appendChunk(const scopeone::core::TimeSeriesChunk& chunk);
        void clear();
        bool displayedRange(int channel, double* minimum, double* maximum) const;
        double displayedFrequency(int channel) const;
        // A message on the screen, such as the error that stopped a source; empty removes it
        void setMessage(const QString& message);

        static QColor channelColor(int index);
        static QString formatValue(double value, const QString& unit);

    signals:
        void runningChanged(bool running);
        // The wheel over the screen turns the timebase, positive steps to a longer one
        void timebaseStepRequested(int steps);
        // The wheel over a channel marker turns its scale, positive steps to a larger one
        void channelScaleStepRequested(int channel, int steps);
        // The markers dragged on the screen, or the trigger level turned with Shift and the wheel
        void triggerLevelChanged(double level);
        void triggerPositionChanged(double position);
        void channelOffsetChanged(int channel, double offset);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;

    private:
        enum class DisplayState
        {
            Roll,
            Triggered,
            Auto,
            Ready
        };

        enum class Handle
        {
            None,
            Cursor,
            TriggerLevel,
            TriggerPosition,
            ChannelOffset
        };

        struct Hit
        {
            Handle handle{Handle::None};
            int index{-1};
        };

        // The extremes and sums of the samples in one slice of time, a fixed fraction of the screen
        struct Bucket
        {
            qint64 index{0};
            double minimum{0.0};
            double maximum{0.0};
            double sum{0.0};
            double squares{0.0};
            qint64 count{0};
        };

        // The received samples of one channel, kept twice. The raw samples cover only the last
        // stretch, for the trigger, for exact frequencies and for screens with few samples. The
        // buckets cover two screens whatever the sample rate, so a long timebase on a fast source
        // draws and measures a few thousand buckets instead of millions of samples.
        // Old raw samples are dropped by moving the start, and the vectors are compacted only now
        // and then, so trimming does not copy on every chunk.
        struct Trace
        {
            QVector<double> times;
            QVector<double> values;
            qsizetype begin{0};
            double interval{0.0};
            std::deque<Bucket> buckets;

            bool empty() const;
            double first() const;
            double last() const;
            double earliest(double bucketSeconds) const;
            qsizetype indexAtOrAfter(double time) const;
            void append(const scopeone::core::TimeSeriesChunk& chunk, double bucketSeconds);
            void addToBucket(double time, double value, double bucketSeconds);
            void trim(double cutoff, qsizetype maximumSamples, qint64 retainedBuckets);
            void rebuildBuckets(double bucketSeconds);
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
            // A raw channel has one sample per point, so its minima and maxima are the same; a
            // bucketed one has one bucket per point
            std::array<QVector<double>, kChannelCount> times;
            std::array<QVector<double>, kChannelCount> minima;
            std::array<QVector<double>, kChannelCount> maxima;
            std::array<bool, kChannelCount> bucketed{};
            std::array<Measurement, kChannelCount> measurements;
        };

        double screenSeconds() const;
        double bucketSeconds() const;
        double frequencyOf(int channel, double start, double end, const Measurement& measurement) const;
        double newestTime() const;
        bool triggerShown() const;
        void refresh();
        void processTrigger();
        void buildFrame(double start, double end, double triggerTime);
        void resetTriggerSearch();
        void updateHysteresis();
        double triggerY() const;
        double channelTagY(int channel) const;
        double divisionsAt(double y) const;
        Hit hitTest(const QPointF& point) const;
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
        // An edge whose screen is still being received
        double m_pendingTrigger{std::numeric_limits<double>::quiet_NaN()};
        double m_scanTime{-std::numeric_limits<double>::infinity()};
        QElapsedTimer m_sinceTrigger;
        QElapsedTimer m_sinceFrequency;
        QElapsedTimer m_sinceHysteresis;
        double m_triggerHysteresis{std::numeric_limits<double>::quiet_NaN()};
        QString m_message;
        std::array<double, kChannelCount> m_frequencies{};
        QPixmap m_gridCache;
        QRectF m_gridScreen;
        DisplayState m_state{DisplayState::Ready};
        bool m_cursorsVisible{false};
        int m_cursorChannel{0};
        // Two time cursors as fractions of the screen width, then two level cursors in divisions
        std::array<double, 4> m_cursors{0.3, 0.7, 2.0, -2.0};
        Hit m_drag;
        int m_wheelAngle{0};
        QRectF m_screen;
    };
}
