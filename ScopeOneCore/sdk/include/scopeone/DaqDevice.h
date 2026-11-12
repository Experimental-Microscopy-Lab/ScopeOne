#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    enum class DaqChannelType
    {
        AnalogInput,
        AnalogOutput,
        DigitalInput,
        DigitalOutput,
        CounterInput,
        CounterOutput
    };

    enum class DaqEdge
    {
        Rising,
        Falling
    };

    enum class DaqState
    {
        Idle,
        Armed,
        Running,
        Error
    };

    enum class DaqTaskDirection
    {
        Input,
        Output
    };

    enum class DaqSampleMode
    {
        Finite,
        Continuous
    };

    struct DaqChannelDescriptor
    {
        QString physicalName;
        DaqChannelType type{DaqChannelType::DigitalInput};
    };

    struct DaqDeviceDescriptor
    {
        QString id;
        QString name;
        QString provider;
        QString product;
        QList<DaqChannelDescriptor> channels;
        QStringList terminals;
    };

    struct DaqPulseTaskConfig
    {
        QString name;
        QString counter;
        QString outputTerminal;
        double frequencyHz{1000.0};
        double dutyCycle{0.5};
        double initialDelaySeconds{0.0};
        QString startTrigger;
        DaqEdge startEdge{DaqEdge::Rising};
        QString timebaseSource;
        quint32 initialDelayTicks{0};
        quint32 lowTicks{0};
        quint32 highTicks{0};
    };

    struct DaqTaskTiming
    {
        QString sampleClock;
        double sampleRateHz{1000.0};
        DaqEdge sampleEdge{DaqEdge::Rising};
        DaqSampleMode sampleMode{DaqSampleMode::Finite};
        quint64 samplesPerChannel{1};
        QString startTrigger;
        DaqEdge startEdge{DaqEdge::Rising};
    };

    struct DaqAnalogTaskConfig
    {
        QString name;
        DaqTaskDirection direction{DaqTaskDirection::Input};
        QStringList channels;
        double minimumVolts{-10.0};
        double maximumVolts{10.0};
        DaqTaskTiming timing;
        QVector<double> outputSamplesByScan;
    };

    struct DaqDigitalTaskConfig
    {
        QString name;
        DaqTaskDirection direction{DaqTaskDirection::Input};
        QStringList lines;
        DaqTaskTiming timing;
        QVector<quint32> outputSamplesByScan;
    };

    struct DaqInputChunk
    {
        QString deviceId;
        QString taskName;
        QStringList channels;
        quint64 firstSample{0};
        double nominalSampleRateHz{0.0};
        QVector<double> analogSamplesByScan;
        QVector<quint32> digitalSamplesByScan;
    };

    struct DaqTerminalRoute
    {
        QString source;
        QString destination;
        bool inverted{false};
    };

    struct DaqRasterScanConfig
    {
        QString name;
        QString lineClock;
        double nominalLineRateHz{1000.0};
        quint32 activeLines{512};
        quint32 flybackLines{16};
        QString yChannel;
        double yStartVolts{-1.0};
        double yEndVolts{1.0};
        QString frameCounter;
        QString lineOutputTerminal;
        QString frameOutputTerminal;
    };

    struct DaqSessionConfig
    {
        QString deviceId;
        QList<DaqRasterScanConfig> rasterScans;
        QList<DaqPulseTaskConfig> pulseTasks;
        QList<DaqAnalogTaskConfig> analogTasks;
        QList<DaqDigitalTaskConfig> digitalTasks;
        QList<DaqTerminalRoute> routes;
    };

    // A data acquisition device. It runs timed analog, digital and counter
    // tasks, puts out scan waveforms and clocks and reads its inputs continuously
    class SCOPEONE_SDK_EXPORT Daq
    {
    public:
        using InputSink = std::function<void(const DaqInputChunk&)>;
        using StateSink = std::function<void(const QString& deviceId,
                                             DaqState state,
                                             const QString& message)>;

        virtual ~Daq();

        virtual QList<DaqDeviceDescriptor> daqDevices() const = 0;
        // The sinks may be called from any thread, and the input sink in the order of the samples
        virtual void setDaqSinks(InputSink input, StateSink state) = 0;
        // Raster scans of the session are already expanded into its tasks
        virtual bool startDaq(const DaqSessionConfig& config,
                              QString* errorMessage = nullptr) = 0;
        virtual void stopDaq(const QString& deviceId) = 0;
    };
}

Q_DECLARE_METATYPE(scopeone::core::DaqState)
Q_DECLARE_METATYPE(scopeone::core::DaqInputChunk)
