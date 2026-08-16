#include "NIDaqmxPlugin.h"

#include <QLibrary>
#include <QMetaObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace scopeone::plugins
{
    using namespace scopeone::core;

    namespace
    {
#ifdef Q_OS_WIN
#define SCOPEONE_DAQMX_CALL __stdcall
        constexpr auto kLibraryName = "nicaiu.dll";
#else
#define SCOPEONE_DAQMX_CALL
        constexpr auto kLibraryName = "libnidaqmx.so";
#endif

        using Int32 = qint32;
        using UInt32 = quint32;
        using UInt64 = quint64;
        using Bool32 = quint32;
        using TaskHandle = void*;

        constexpr Int32 kRising = 10280;
        constexpr Int32 kFalling = 10171;
        constexpr Int32 kHertz = 10373;
        constexpr Int32 kLow = 10214;
        constexpr Int32 kContinuousSamples = 10123;
        constexpr Int32 kFiniteSamples = 10178;
        constexpr Int32 kDefaultTerminalConfiguration = -1;
        constexpr Int32 kVolts = 10348;
        constexpr Int32 kChannelForAllLines = 1;
        constexpr Int32 kGroupByScanNumber = 1;
        constexpr Int32 kDoNotInvert = 0;
        constexpr Int32 kInvertPolarity = 1;

        struct DaqmxApi
        {
            using GetString = Int32 (SCOPEONE_DAQMX_CALL*)(char*, UInt32);
            using GetDeviceString = Int32 (SCOPEONE_DAQMX_CALL*)(const char*, char*, UInt32);
            using CreateTask = Int32 (SCOPEONE_DAQMX_CALL*)(const char*, TaskHandle*);
            using ClearTask = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle);
            using StartTask = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle);
            using StopTask = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle);
            using CreatePulse = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                             const char*,
                                                             const char*,
                                                             Int32,
                                                             Int32,
                                                             double,
                                                             double,
                                                             double);
            using CreatePulseTicks = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                  const char*,
                                                                  const char*,
                                                                  const char*,
                                                                  Int32,
                                                                  UInt32,
                                                                  UInt32,
                                                                  UInt32);
            using ConfigureImplicit = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                   Int32,
                                                                   UInt64);
            using ConfigureTrigger = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                  const char*,
                                                                  Int32);
            using SetChannelString = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                  const char*,
                                                                  const char*);
            using RouteTerminal = Int32 (SCOPEONE_DAQMX_CALL*)(const char*,
                                                               const char*,
                                                               Int32);
            using DisconnectTerminal = Int32 (SCOPEONE_DAQMX_CALL*)(const char*,
                                                                    const char*);
            using CreateAiVoltage = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                 const char*,
                                                                 const char*,
                                                                 Int32,
                                                                 double,
                                                                 double,
                                                                 Int32,
                                                                 const char*);
            using CreateAoVoltage = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                 const char*,
                                                                 const char*,
                                                                 double,
                                                                 double,
                                                                 Int32,
                                                                 const char*);
            using CreateDigital = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                               const char*,
                                                               const char*,
                                                               Int32);
            using ConfigureSampleClock = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                      const char*,
                                                                      double,
                                                                      Int32,
                                                                      Int32,
                                                                      UInt64);
            using WriteAnalog = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                             Int32,
                                                             Bool32,
                                                             double,
                                                             Int32,
                                                             const double*,
                                                             Int32*,
                                                             void*);
            using WriteDigital = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                              Int32,
                                                              Bool32,
                                                              double,
                                                              Int32,
                                                              const UInt32*,
                                                              Int32*,
                                                              void*);
            using AvailableSamples = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                                  UInt32*);
            using ReadAnalog = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                            Int32,
                                                            double,
                                                            Int32,
                                                            double*,
                                                            UInt32,
                                                            Int32*,
                                                            void*);
            using ReadDigital = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle,
                                                             Int32,
                                                             double,
                                                             Int32,
                                                             UInt32*,
                                                             UInt32,
                                                             Int32*,
                                                             void*);

            QLibrary library;
            GetString getSystemDeviceNames{nullptr};
            GetString getExtendedErrorInfo{nullptr};
            GetDeviceString getProductType{nullptr};
            GetDeviceString getAiChannels{nullptr};
            GetDeviceString getAoChannels{nullptr};
            GetDeviceString getDiLines{nullptr};
            GetDeviceString getDoLines{nullptr};
            GetDeviceString getCiChannels{nullptr};
            GetDeviceString getCoChannels{nullptr};
            GetDeviceString getTerminals{nullptr};
            CreateTask createTask{nullptr};
            ClearTask clearTask{nullptr};
            StartTask startTask{nullptr};
            StopTask stopTask{nullptr};
            CreatePulse createPulseChannel{nullptr};
            CreatePulseTicks createPulseTicksChannel{nullptr};
            ConfigureImplicit configureImplicitTiming{nullptr};
            ConfigureTrigger configureStartTrigger{nullptr};
            SetChannelString setCounterOutputTerminal{nullptr};
            RouteTerminal connectTerminals{nullptr};
            DisconnectTerminal disconnectTerminals{nullptr};
            CreateAiVoltage createAiVoltageChannel{nullptr};
            CreateAoVoltage createAoVoltageChannel{nullptr};
            CreateDigital createDiChannel{nullptr};
            CreateDigital createDoChannel{nullptr};
            ConfigureSampleClock configureSampleClock{nullptr};
            WriteAnalog writeAnalog{nullptr};
            WriteDigital writeDigital{nullptr};
            AvailableSamples availableSamples{nullptr};
            ReadAnalog readAnalog{nullptr};
            ReadDigital readDigital{nullptr};

            DaqmxApi()
                : library(QString::fromLatin1(kLibraryName))
            {
            }

            template<typename Function>
            bool resolve(Function& function, const char* name)
            {
                function = reinterpret_cast<Function>(library.resolve(name));
                return function != nullptr;
            }

            bool load(bool controllerFunctions, QString* errorMessage = nullptr)
            {
                if (!library.load())
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("NI-DAQmx runtime is unavailable: %1")
                                            .arg(library.errorString());
                    }
                    return false;
                }

                bool ok = resolve(getSystemDeviceNames, "DAQmxGetSysDevNames")
                    && resolve(getExtendedErrorInfo, "DAQmxGetExtendedErrorInfo")
                    && resolve(getProductType, "DAQmxGetDevProductType")
                    && resolve(getAiChannels, "DAQmxGetDevAIPhysicalChans")
                    && resolve(getAoChannels, "DAQmxGetDevAOPhysicalChans")
                    && resolve(getDiLines, "DAQmxGetDevDILines")
                    && resolve(getDoLines, "DAQmxGetDevDOLines")
                    && resolve(getCiChannels, "DAQmxGetDevCIPhysicalChans")
                    && resolve(getCoChannels, "DAQmxGetDevCOPhysicalChans")
                    && resolve(getTerminals, "DAQmxGetDevTerminals");
                if (controllerFunctions)
                {
                    ok = ok
                        && resolve(createTask, "DAQmxCreateTask")
                        && resolve(clearTask, "DAQmxClearTask")
                        && resolve(startTask, "DAQmxStartTask")
                        && resolve(stopTask, "DAQmxStopTask")
                        && resolve(createPulseChannel, "DAQmxCreateCOPulseChanFreq")
                        && resolve(createPulseTicksChannel, "DAQmxCreateCOPulseChanTicks")
                        && resolve(configureImplicitTiming, "DAQmxCfgImplicitTiming")
                        && resolve(configureStartTrigger, "DAQmxCfgDigEdgeStartTrig")
                        && resolve(setCounterOutputTerminal, "DAQmxSetCOPulseTerm")
                        && resolve(connectTerminals, "DAQmxConnectTerms")
                        && resolve(disconnectTerminals, "DAQmxDisconnectTerms")
                        && resolve(createAiVoltageChannel, "DAQmxCreateAIVoltageChan")
                        && resolve(createAoVoltageChannel, "DAQmxCreateAOVoltageChan")
                        && resolve(createDiChannel, "DAQmxCreateDIChan")
                        && resolve(createDoChannel, "DAQmxCreateDOChan")
                        && resolve(configureSampleClock, "DAQmxCfgSampClkTiming")
                        && resolve(writeAnalog, "DAQmxWriteAnalogF64")
                        && resolve(writeDigital, "DAQmxWriteDigitalU32")
                        && resolve(availableSamples, "DAQmxGetReadAvailSampPerChan")
                        && resolve(readAnalog, "DAQmxReadAnalogF64")
                        && resolve(readDigital, "DAQmxReadDigitalU32");
                }
                if (!ok && errorMessage)
                {
                    *errorMessage = QStringLiteral("NI-DAQmx runtime is missing required functions");
                }
                return ok;
            }

            QString error(Int32 code) const
            {
                std::array<char, 4096> buffer{};
                if (getExtendedErrorInfo)
                {
                    getExtendedErrorInfo(buffer.data(), static_cast<UInt32>(buffer.size()));
                }
                const QString detail = QString::fromLocal8Bit(buffer.data()).trimmed();
                return detail.isEmpty()
                           ? QStringLiteral("NI-DAQmx error %1").arg(code)
                           : detail;
            }
        };

        QStringList splitNames(const QByteArray& value)
        {
            QStringList result;
            for (const QString& name : QString::fromLocal8Bit(value).split(
                     QLatin1Char(','), Qt::SkipEmptyParts))
            {
                const QString trimmed = name.trimmed();
                if (!trimmed.isEmpty())
                {
                    result.append(trimmed);
                }
            }
            return result;
        }

        QString readSystemString(DaqmxApi::GetString function)
        {
            const Int32 size = function(nullptr, 0);
            if (size <= 0)
            {
                return {};
            }
            QByteArray buffer(size, '\0');
            return function(buffer.data(), static_cast<UInt32>(buffer.size())) < 0
                       ? QString()
                       : QString::fromLocal8Bit(buffer.constData());
        }

        QString readDeviceString(DaqmxApi::GetDeviceString function,
                                 const QString& device)
        {
            const QByteArray encoded = device.toLocal8Bit();
            const Int32 size = function(encoded.constData(), nullptr, 0);
            if (size <= 0)
            {
                return {};
            }
            QByteArray buffer(size, '\0');
            return function(encoded.constData(), buffer.data(),
                            static_cast<UInt32>(buffer.size())) < 0
                       ? QString()
                       : QString::fromLocal8Bit(buffer.constData());
        }

        QString nativeDeviceId(const QString& id)
        {
            return id.startsWith(QStringLiteral("ni:")) ? id.mid(3) : id;
        }

        QString absoluteTerminal(const QString& device, const QString& terminal)
        {
            const QString trimmed = terminal.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('/')))
            {
                return trimmed;
            }
            if (trimmed.startsWith(device + QLatin1Char('/'), Qt::CaseInsensitive))
            {
                return QLatin1Char('/') + trimmed;
            }
            return QStringLiteral("/%1/%2").arg(device, trimmed);
        }

        class NIDaqmxController final : public DaqController
        {
        public:
            explicit NIDaqmxController(QString device, QObject* parent)
                : DaqController(parent)
                  , m_device(std::move(device))
            {
            }

            ~NIDaqmxController() override
            {
                stop();
            }

            bool start(const DaqSessionConfig& config,
                       QString* errorMessage) override
            {
                if (m_state == DaqState::Armed || m_state == DaqState::Running)
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("DAQ device is already active");
                    }
                    return false;
                }
                QString loadError;
                if (!m_api.load(true, &loadError))
                {
                    return fail(loadError, errorMessage);
                }

                emitState(DaqState::Armed, QStringLiteral("Configuring hardware tasks"));
                for (const DaqTerminalRoute& route : config.routes)
                {
                    const QString source = absoluteTerminal(m_device, route.source);
                    const QString destination = absoluteTerminal(m_device, route.destination);
                    if (source.isEmpty() || destination.isEmpty())
                    {
                        return fail(QStringLiteral("DAQ terminal route is incomplete"),
                                    errorMessage);
                    }
                    const QByteArray encodedSource = source.toLocal8Bit();
                    const QByteArray encodedDestination = destination.toLocal8Bit();
                    const Int32 result = m_api.connectTerminals(
                        encodedSource.constData(), encodedDestination.constData(),
                        route.inverted ? kInvertPolarity : kDoNotInvert);
                    if (result < 0)
                    {
                        const QString message = m_api.error(result);
                        return fail(message, errorMessage);
                    }
                    m_routes.push_back({source, destination});
                }

                for (int index = 0; index < config.pulseTasks.size(); ++index)
                {
                    const DaqPulseTaskConfig& pulse = config.pulseTasks[index];
                    const bool externalTicks = !pulse.timebaseSource.trimmed().isEmpty();
                    const bool validFrequency = std::isfinite(pulse.frequencyHz)
                        && pulse.frequencyHz > 0.0
                        && std::isfinite(pulse.dutyCycle)
                        && pulse.dutyCycle > 0.0
                        && pulse.dutyCycle < 1.0
                        && std::isfinite(pulse.initialDelaySeconds)
                        && pulse.initialDelaySeconds >= 0.0;
                    const bool validTicks = pulse.lowTicks > 0 && pulse.highTicks > 0;
                    if ((!externalTicks && !validFrequency)
                        || (externalTicks && !validTicks))
                    {
                        return fail(QStringLiteral("Invalid DAQ pulse timing"), errorMessage);
                    }
                    const QString counter = pulse.counter.trimmed();
                    if (counter.isEmpty())
                    {
                        return fail(QStringLiteral("DAQ pulse task has no counter"), errorMessage);
                    }

                    const QByteArray taskName = (pulse.name.trimmed().isEmpty()
                                                     ? QStringLiteral("ScopeOne Pulse %1")
                                                           .arg(index + 1)
                                                     : pulse.name.trimmed()).toLocal8Bit();
                    TaskHandle handle = nullptr;
                    Int32 result = m_api.createTask(taskName.constData(), &handle);
                    if (result >= 0)
                    {
                        const QByteArray channel = counter.toLocal8Bit();
                        if (externalTicks)
                        {
                            const QByteArray timebase = absoluteTerminal(
                                m_device, pulse.timebaseSource).toLocal8Bit();
                            result = m_api.createPulseTicksChannel(
                                handle, channel.constData(), "", timebase.constData(),
                                kLow, pulse.initialDelayTicks,
                                pulse.lowTicks, pulse.highTicks);
                        }
                        else
                        {
                            result = m_api.createPulseChannel(
                                handle, channel.constData(), "", kHertz, kLow,
                                pulse.initialDelaySeconds, pulse.frequencyHz,
                                pulse.dutyCycle);
                        }
                        if (result >= 0 && !pulse.outputTerminal.trimmed().isEmpty())
                        {
                            const QByteArray terminal = absoluteTerminal(
                                m_device, pulse.outputTerminal).toLocal8Bit();
                            result = m_api.setCounterOutputTerminal(
                                handle, channel.constData(), terminal.constData());
                        }
                    }
                    if (result >= 0)
                    {
                        result = m_api.configureImplicitTiming(
                            handle, kContinuousSamples, 1000);
                    }
                    if (result >= 0 && !pulse.startTrigger.trimmed().isEmpty())
                    {
                        const QByteArray trigger = absoluteTerminal(
                            m_device, pulse.startTrigger).toLocal8Bit();
                        result = m_api.configureStartTrigger(
                            handle, trigger.constData(),
                            pulse.startEdge == DaqEdge::Rising ? kRising : kFalling);
                    }
                    if (result < 0)
                    {
                        if (handle)
                        {
                            m_api.clearTask(handle);
                        }
                        const QString message = m_api.error(result);
                        return fail(message, errorMessage);
                    }
                    ActiveTask task;
                    task.handle = handle;
                    task.triggered = !pulse.startTrigger.trimmed().isEmpty();
                    task.producer = true;
                    m_tasks.push_back(std::move(task));
                }

                for (int index = 0; index < config.analogTasks.size(); ++index)
                {
                    QString message;
                    if (!createAnalogTask(config.analogTasks[index], index, message))
                    {
                        return fail(message, errorMessage);
                    }
                }
                for (int index = 0; index < config.digitalTasks.size(); ++index)
                {
                    QString message;
                    if (!createDigitalTask(config.digitalTasks[index], index, message))
                    {
                        return fail(message, errorMessage);
                    }
                }

                // Arm externally triggered tasks and inputs before free-running outputs.
                for (int phase = 0; phase < 3; ++phase)
                {
                    for (const ActiveTask& task : m_tasks)
                    {
                        const int taskPhase = task.triggered ? 0 : (task.producer ? 2 : 1);
                        if (taskPhase != phase)
                        {
                            continue;
                        }
                        const Int32 result = m_api.startTask(task.handle);
                        if (result < 0)
                        {
                            const QString message = m_api.error(result);
                            return fail(message, errorMessage);
                        }
                    }
                }
                const bool hasInput = std::any_of(
                    m_tasks.cbegin(), m_tasks.cend(),
                    [](const ActiveTask& task) { return task.inputKind != InputKind::None; });
                if (hasInput)
                {
                    m_reading.store(true);
                    m_reader = std::thread([this]() { readInputs(); });
                }
                emitState(DaqState::Running, QStringLiteral("DAQ session is active"));
                return true;
            }

            void stop() override
            {
                releaseHardware();
                if (m_state != DaqState::Idle)
                {
                    emitState(DaqState::Idle, QStringLiteral("DAQ device is idle"));
                }
            }

            DaqState state() const override
            {
                return m_state;
            }

            QString stateMessage() const override
            {
                return m_message;
            }

        private:
            enum class InputKind
            {
                None,
                Analog,
                Digital
            };

            struct ActiveTask
            {
                TaskHandle handle{nullptr};
                bool triggered{false};
                bool producer{false};
                InputKind inputKind{InputKind::None};
                QString name;
                QStringList channels;
                quint64 nextSample{0};
                double nominalSampleRateHz{0.0};
            };

            bool configureTiming(TaskHandle handle,
                                 const DaqTaskTiming& timing,
                                 QString& errorMessage)
            {
                if (!std::isfinite(timing.sampleRateHz)
                    || timing.sampleRateHz <= 0.0
                    || timing.samplesPerChannel == 0)
                {
                    errorMessage = QStringLiteral("Invalid DAQ task timing");
                    return false;
                }
                const QByteArray clock = absoluteTerminal(
                    m_device, timing.sampleClock).toLocal8Bit();
                Int32 result = m_api.configureSampleClock(
                    handle,
                    clock.constData(),
                    timing.sampleRateHz,
                    timing.sampleEdge == DaqEdge::Rising ? kRising : kFalling,
                    timing.sampleMode == DaqSampleMode::Continuous
                        ? kContinuousSamples
                        : kFiniteSamples,
                    timing.samplesPerChannel);
                if (result >= 0 && !timing.startTrigger.trimmed().isEmpty())
                {
                    const QByteArray trigger = absoluteTerminal(
                        m_device, timing.startTrigger).toLocal8Bit();
                    result = m_api.configureStartTrigger(
                        handle, trigger.constData(),
                        timing.startEdge == DaqEdge::Rising ? kRising : kFalling);
                }
                if (result < 0)
                {
                    errorMessage = m_api.error(result);
                    return false;
                }
                return true;
            }

            bool createAnalogTask(const DaqAnalogTaskConfig& config,
                                  int index,
                                  QString& errorMessage)
            {
                if (config.channels.isEmpty()
                    || !std::isfinite(config.minimumVolts)
                    || !std::isfinite(config.maximumVolts)
                    || config.minimumVolts >= config.maximumVolts)
                {
                    errorMessage = QStringLiteral("Invalid analog DAQ task");
                    return false;
                }
                const quint64 expectedSamples = config.timing.samplesPerChannel
                    * static_cast<quint64>(config.channels.size());
                if (config.direction == DaqTaskDirection::Output
                    && (config.timing.samplesPerChannel
                            > static_cast<quint64>(std::numeric_limits<Int32>::max())
                        || static_cast<quint64>(config.outputSamplesByScan.size())
                            != expectedSamples))
                {
                    errorMessage = QStringLiteral(
                        "Analog output data must contain samples-per-channel times channel-count values");
                    return false;
                }

                const QString name = config.name.trimmed().isEmpty()
                                         ? QStringLiteral("Analog Task %1").arg(index + 1)
                                         : config.name.trimmed();
                TaskHandle handle = nullptr;
                Int32 result = m_api.createTask(name.toLocal8Bit().constData(), &handle);
                const QByteArray channels = config.channels.join(QLatin1Char(',')).toLocal8Bit();
                if (result >= 0 && config.direction == DaqTaskDirection::Input)
                {
                    result = m_api.createAiVoltageChannel(
                        handle, channels.constData(), "", kDefaultTerminalConfiguration,
                        config.minimumVolts, config.maximumVolts, kVolts, nullptr);
                }
                else if (result >= 0)
                {
                    result = m_api.createAoVoltageChannel(
                        handle, channels.constData(), "", config.minimumVolts,
                        config.maximumVolts, kVolts, nullptr);
                }
                if (result >= 0 && !configureTiming(handle, config.timing, errorMessage))
                {
                    result = -1;
                }
                if (result >= 0 && config.direction == DaqTaskDirection::Output)
                {
                    Int32 written = 0;
                    result = m_api.writeAnalog(
                        handle, static_cast<Int32>(config.timing.samplesPerChannel),
                        0, 10.0, kGroupByScanNumber,
                        config.outputSamplesByScan.constData(), &written, nullptr);
                    if (result >= 0
                        && written != static_cast<Int32>(config.timing.samplesPerChannel))
                    {
                        errorMessage = QStringLiteral("NI-DAQmx accepted only part of the analog buffer");
                        result = -1;
                    }
                }
                if (result < 0)
                {
                    if (errorMessage.isEmpty())
                    {
                        errorMessage = m_api.error(result);
                    }
                    if (handle)
                    {
                        m_api.clearTask(handle);
                    }
                    return false;
                }

                ActiveTask task;
                task.handle = handle;
                task.triggered = !config.timing.startTrigger.trimmed().isEmpty();
                task.producer = config.direction == DaqTaskDirection::Output;
                task.inputKind = config.direction == DaqTaskDirection::Input
                                     ? InputKind::Analog
                                     : InputKind::None;
                task.name = name;
                task.channels = config.channels;
                task.nominalSampleRateHz = config.timing.sampleRateHz;
                m_tasks.push_back(std::move(task));
                return true;
            }

            bool createDigitalTask(const DaqDigitalTaskConfig& config,
                                   int index,
                                   QString& errorMessage)
            {
                if (config.lines.isEmpty())
                {
                    errorMessage = QStringLiteral("Digital DAQ task has no lines");
                    return false;
                }
                if (config.direction == DaqTaskDirection::Output
                    && (config.timing.samplesPerChannel
                            > static_cast<quint64>(std::numeric_limits<Int32>::max())
                        || static_cast<quint64>(config.outputSamplesByScan.size())
                            != config.timing.samplesPerChannel))
                {
                    errorMessage = QStringLiteral(
                        "Digital output data must contain one port value per sample");
                    return false;
                }

                const QString name = config.name.trimmed().isEmpty()
                                         ? QStringLiteral("Digital Task %1").arg(index + 1)
                                         : config.name.trimmed();
                TaskHandle handle = nullptr;
                Int32 result = m_api.createTask(name.toLocal8Bit().constData(), &handle);
                const QByteArray lines = config.lines.join(QLatin1Char(',')).toLocal8Bit();
                if (result >= 0)
                {
                    result = config.direction == DaqTaskDirection::Input
                                 ? m_api.createDiChannel(handle, lines.constData(), "",
                                                         kChannelForAllLines)
                                 : m_api.createDoChannel(handle, lines.constData(), "",
                                                         kChannelForAllLines);
                }
                if (result >= 0 && !configureTiming(handle, config.timing, errorMessage))
                {
                    result = -1;
                }
                if (result >= 0 && config.direction == DaqTaskDirection::Output)
                {
                    Int32 written = 0;
                    result = m_api.writeDigital(
                        handle, static_cast<Int32>(config.timing.samplesPerChannel),
                        0, 10.0, kGroupByScanNumber,
                        config.outputSamplesByScan.constData(), &written, nullptr);
                    if (result >= 0
                        && written != static_cast<Int32>(config.timing.samplesPerChannel))
                    {
                        errorMessage = QStringLiteral("NI-DAQmx accepted only part of the digital buffer");
                        result = -1;
                    }
                }
                if (result < 0)
                {
                    if (errorMessage.isEmpty())
                    {
                        errorMessage = m_api.error(result);
                    }
                    if (handle)
                    {
                        m_api.clearTask(handle);
                    }
                    return false;
                }

                ActiveTask task;
                task.handle = handle;
                task.triggered = !config.timing.startTrigger.trimmed().isEmpty();
                task.producer = config.direction == DaqTaskDirection::Output;
                task.inputKind = config.direction == DaqTaskDirection::Input
                                     ? InputKind::Digital
                                     : InputKind::None;
                task.name = name;
                task.channels = config.lines;
                task.nominalSampleRateHz = config.timing.sampleRateHz;
                m_tasks.push_back(std::move(task));
                return true;
            }

            void readInputs()
            {
                constexpr UInt32 kSamplesPerRead = 65536;
                while (m_reading.load())
                {
                    for (ActiveTask& task : m_tasks)
                    {
                        if (task.inputKind == InputKind::None)
                        {
                            continue;
                        }
                        UInt32 available = 0;
                        Int32 result = m_api.availableSamples(task.handle, &available);
                        if (result < 0)
                        {
                            reportReadError(m_api.error(result));
                            return;
                        }
                        if (available == 0)
                        {
                            continue;
                        }
                        const UInt32 samples = std::min(available, kSamplesPerRead);
                        DaqInputChunk chunk;
                        chunk.deviceId = QStringLiteral("ni:%1").arg(m_device);
                        chunk.taskName = task.name;
                        chunk.channels = task.channels;
                        chunk.firstSample = task.nextSample;
                        chunk.nominalSampleRateHz = task.nominalSampleRateHz;
                        Int32 samplesRead = 0;
                        if (task.inputKind == InputKind::Analog)
                        {
                            chunk.analogSamplesByScan.resize(
                                static_cast<qsizetype>(samples * task.channels.size()));
                            result = m_api.readAnalog(
                                task.handle, static_cast<Int32>(samples), 0.0,
                                kGroupByScanNumber, chunk.analogSamplesByScan.data(),
                                static_cast<UInt32>(chunk.analogSamplesByScan.size()),
                                &samplesRead, nullptr);
                            chunk.analogSamplesByScan.resize(
                                static_cast<qsizetype>(samplesRead * task.channels.size()));
                        }
                        else
                        {
                            chunk.digitalSamplesByScan.resize(
                                static_cast<qsizetype>(samples));
                            result = m_api.readDigital(
                                task.handle, static_cast<Int32>(samples), 0.0,
                                kGroupByScanNumber, chunk.digitalSamplesByScan.data(),
                                static_cast<UInt32>(chunk.digitalSamplesByScan.size()),
                                &samplesRead, nullptr);
                            chunk.digitalSamplesByScan.resize(samplesRead);
                        }
                        if (result >= 0 && samplesRead > 0)
                        {
                            task.nextSample += static_cast<quint64>(samplesRead);
                            emit inputDataReady(chunk);
                        }
                        else if (result < 0)
                        {
                            reportReadError(m_api.error(result));
                            return;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }

            void releaseHardware()
            {
                m_reading.store(false);
                if (m_reader.joinable())
                {
                    m_reader.join();
                }
                for (auto it = m_tasks.rbegin(); it != m_tasks.rend(); ++it)
                {
                    m_api.stopTask(it->handle);
                    m_api.clearTask(it->handle);
                }
                m_tasks.clear();
                for (auto it = m_routes.rbegin(); it != m_routes.rend(); ++it)
                {
                    const QByteArray source = it->first.toLocal8Bit();
                    const QByteArray destination = it->second.toLocal8Bit();
                    m_api.disconnectTerminals(source.constData(), destination.constData());
                }
                m_routes.clear();
            }

            void reportReadError(const QString& message)
            {
                QMetaObject::invokeMethod(this, [this, message]()
                {
                    if (m_state == DaqState::Armed || m_state == DaqState::Running)
                    {
                        fail(message, nullptr);
                    }
                }, Qt::QueuedConnection);
            }

            bool fail(const QString& message, QString* errorMessage)
            {
                releaseHardware();
                if (errorMessage)
                {
                    *errorMessage = message;
                }
                emitState(DaqState::Error, message);
                emit controllerError(message);
                return false;
            }

            void emitState(DaqState state, const QString& message)
            {
                m_state = state;
                m_message = message;
                emit stateChanged(state, message);
            }

            QString m_device;
            DaqmxApi m_api;
            std::vector<ActiveTask> m_tasks;
            std::vector<std::pair<QString, QString>> m_routes;
            std::atomic_bool m_reading{false};
            std::thread m_reader;
            DaqState m_state{DaqState::Idle};
            QString m_message{QStringLiteral("DAQ device is idle")};
        };

        void appendChannels(DaqDeviceDescriptor& descriptor,
                            const QString& names,
                            DaqChannelType type)
        {
            for (const QString& name : splitNames(names.toLocal8Bit()))
            {
                descriptor.channels.append({name, type});
            }
        }
    }

    QList<DaqDeviceDescriptor> NIDaqmxPlugin::devices() const
    {
        DaqmxApi api;
        if (!api.load(false))
        {
            return {};
        }

        QList<DaqDeviceDescriptor> result;
        for (const QString& device : splitNames(
                 readSystemString(api.getSystemDeviceNames).toLocal8Bit()))
        {
            DaqDeviceDescriptor descriptor;
            descriptor.id = QStringLiteral("ni:%1").arg(device);
            descriptor.name = device;
            descriptor.provider = QStringLiteral("National Instruments");
            descriptor.product = readDeviceString(api.getProductType, device);
            appendChannels(descriptor, readDeviceString(api.getAiChannels, device),
                           DaqChannelType::AnalogInput);
            appendChannels(descriptor, readDeviceString(api.getAoChannels, device),
                           DaqChannelType::AnalogOutput);
            appendChannels(descriptor, readDeviceString(api.getDiLines, device),
                           DaqChannelType::DigitalInput);
            appendChannels(descriptor, readDeviceString(api.getDoLines, device),
                           DaqChannelType::DigitalOutput);
            appendChannels(descriptor, readDeviceString(api.getCiChannels, device),
                           DaqChannelType::CounterInput);
            appendChannels(descriptor, readDeviceString(api.getCoChannels, device),
                           DaqChannelType::CounterOutput);
            descriptor.terminals = splitNames(
                readDeviceString(api.getTerminals, device).toLocal8Bit());
            result.append(std::move(descriptor));
        }
        return result;
    }

    DaqController* NIDaqmxPlugin::createController(const QString& deviceId,
                                                   QObject* parent)
    {
        const QString device = nativeDeviceId(deviceId.trimmed());
        return device.isEmpty() ? nullptr : new NIDaqmxController(device, parent);
    }
}
