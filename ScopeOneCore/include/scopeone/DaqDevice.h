#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtPlugin>

#include "scopeone/scopeone_core_export.h"

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

    class SCOPEONE_CORE_EXPORT DaqController : public QObject
    {
        Q_OBJECT

    public:
        explicit DaqController(QObject* parent = nullptr);
        ~DaqController() override;

        virtual bool start(const DaqSessionConfig& config,
                           QString* errorMessage = nullptr) = 0;
        virtual void stop() = 0;
        virtual DaqState state() const = 0;
        virtual QString stateMessage() const = 0;

    signals:
        void stateChanged(scopeone::core::DaqState state,
                          const QString& message);
        void controllerError(const QString& errorMessage);
        void inputDataReady(const scopeone::core::DaqInputChunk& chunk);
    };

    class DaqDevicePlugin
    {
    public:
        virtual ~DaqDevicePlugin() = default;
        virtual QList<DaqDeviceDescriptor> devices() const = 0;
        virtual DaqController* createController(const QString& deviceId,
                                                QObject* parent = nullptr) = 0;
    };
}

#define SCOPEONE_DAQ_DEVICE_PLUGIN_IID "org.scopeone.DaqDevicePlugin/1.0"
Q_DECLARE_INTERFACE(scopeone::core::DaqDevicePlugin, SCOPEONE_DAQ_DEVICE_PLUGIN_IID)

Q_DECLARE_METATYPE(scopeone::core::DaqState)
Q_DECLARE_METATYPE(scopeone::core::DaqInputChunk)
