// The driver for National Instruments DAQ devices. It loads the NI-DAQmx runtime when
// DriverHost starts it, so ScopeOne itself never loads the vendor library, and it runs the
// timed tasks of a DAQ session: scan waveforms and clocks out, analog and digital inputs read
// continuously on a thread of their own.
#include "scopeone/DriverSDK.h"

#include <QHash>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ni_daqmx
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
            using ConfigureImplicit = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle, Int32, UInt64);
            using ConfigureTrigger = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle, const char*, Int32);
            using SetChannelString = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle, const char*, const char*);
            using RouteTerminal = Int32 (SCOPEONE_DAQMX_CALL*)(const char*, const char*, Int32);
            using DisconnectTerminal = Int32 (SCOPEONE_DAQMX_CALL*)(const char*, const char*);
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
            using AvailableSamples = Int32 (SCOPEONE_DAQMX_CALL*)(TaskHandle, UInt32*);
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

            QLibrary library{QString::fromLatin1(kLibraryName)};
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
            CreatePulse createPulse{nullptr};
            CreatePulseTicks createPulseTicks{nullptr};
            ConfigureImplicit configureImplicit{nullptr};
            ConfigureTrigger configureTrigger{nullptr};
            SetChannelString setChannelString{nullptr};
            RouteTerminal routeTerminal{nullptr};
            DisconnectTerminal disconnectTerminal{nullptr};
            CreateAiVoltage createAiVoltage{nullptr};
            CreateAoVoltage createAoVoltage{nullptr};
            CreateDigital createDi{nullptr};
            CreateDigital createDo{nullptr};
            ConfigureSampleClock configureSampleClock{nullptr};
            WriteAnalog writeAnalog{nullptr};
            WriteDigital writeDigital{nullptr};
            AvailableSamples availableSamples{nullptr};
            ReadAnalog readAnalog{nullptr};
            ReadDigital readDigital{nullptr};

            template<typename Function>
            bool resolve(Function& function, const char* name)
            {
                function = reinterpret_cast<Function>(library.resolve(name));
                return function != nullptr;
            }

            bool load(bool controllerFunctions, QString* errorMessage = nullptr)
            {
                if (!library.isLoaded() && !library.load())
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
                        && resolve(createPulse, "DAQmxCreateCOPulseChanFreq")
                        && resolve(createPulseTicks, "DAQmxCreateCOPulseChanTicks")
                        && resolve(configureImplicit, "DAQmxCfgImplicitTiming")
                        && resolve(configureTrigger, "DAQmxCfgDigEdgeStartTrig")
                        && resolve(setChannelString, "DAQmxSetCOPulseTerm")
                        && resolve(routeTerminal, "DAQmxConnectTerms")
                        && resolve(disconnectTerminal, "DAQmxDisconnectTerms")
                        && resolve(createAiVoltage, "DAQmxCreateAIVoltageChan")
                        && resolve(createAoVoltage, "DAQmxCreateAOVoltageChan")
                        && resolve(createDi, "DAQmxCreateDIChan")
                        && resolve(createDo, "DAQmxCreateDOChan")
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
            QStringList names;
            for (const QString& name : QString::fromLocal8Bit(value).split(
                     QLatin1Char(','), Qt::SkipEmptyParts))
            {
                names.append(name.trimmed());
            }
            return names;
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
            const QString value = terminal.trimmed();
            if (value.isEmpty() || value.startsWith(QLatin1Char('/')))
            {
                return value;
            }
            return value.startsWith(device + QLatin1Char('/'), Qt::CaseInsensitive)
                       ? QLatin1Char('/') + value
                       : QStringLiteral("/%1/%2").arg(device, value);
        }

        // One DAQ device and the tasks of its session
        class NIDaqmxController final
        {
        public:
            using InputSink = std::function<void(const DaqInputChunk&)>;
            using StateSink = std::function<void(DaqState, const QString&)>;

            NIDaqmxController(QString device, InputSink input, StateSink state)
                : m_device(std::move(device))
                  , m_input(std::move(input))
                  , m_stateSink(std::move(state))
            {
            }

            ~NIDaqmxController()
            {
                releaseHardware();
            }

            bool start(const DaqSessionConfig& config,
                       QString* errorMessage)
            {
                if (m_state == DaqState::Armed || m_state == DaqState::Running)
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("DAQ device is already active");
                    }
                    return false;
                }
                // A read error ends the reader and leaves the tasks of that session to be cleared here
                releaseHardware();

                QString loadError;
                if (!m_api.load(true, &loadError))
                {
                    return fail(loadError, errorMessage);
                }
                emitState(DaqState::Armed, QStringLiteral("Configuring hardware tasks"));

                for (const DaqTerminalRoute& route : config.routes)
                {
                    const QByteArray source = absoluteTerminal(m_device, route.source).toLocal8Bit();
                    const QByteArray destination = absoluteTerminal(m_device, route.destination).toLocal8Bit();
                    const Int32 result = m_api.routeTerminal(
                        source.constData(),
                        destination.constData(),
                        route.inverted ? kInvertPolarity : kDoNotInvert);
                    if (result < 0)
                    {
                        return fail(m_api.error(result), errorMessage);
                    }
                    m_routes.push_back({source, destination});
                }

                QString taskError;
                for (int index = 0; index < config.pulseTasks.size(); ++index)
                {
                    if (!createPulseTask(config.pulseTasks[index], index, taskError))
                    {
                        return fail(taskError, errorMessage);
                    }
                }
                for (int index = 0; index < config.analogTasks.size(); ++index)
                {
                    if (!createAnalogTask(config.analogTasks[index], index, taskError))
                    {
                        return fail(taskError, errorMessage);
                    }
                }
                for (int index = 0; index < config.digitalTasks.size(); ++index)
                {
                    if (!createDigitalTask(config.digitalTasks[index], index, taskError))
                    {
                        return fail(taskError, errorMessage);
                    }
                }
                if (m_tasks.empty())
                {
                    return fail(QStringLiteral("DAQ session has no tasks"), errorMessage);
                }

                for (ActiveTask& task : m_tasks)
                {
                    const Int32 result = m_api.startTask(task.handle);
                    if (result < 0)
                    {
                        return fail(m_api.error(result), errorMessage);
                    }
                }
                if (std::any_of(m_tasks.cbegin(), m_tasks.cend(),
                                [](const ActiveTask& task)
                                {
                                    return task.inputKind != InputKind::None;
                                }))
                {
                    m_reading.store(true);
                    m_reader = std::thread([this]() { readInputs(); });
                }
                emitState(DaqState::Running, QStringLiteral("DAQ session is active"));
                return true;
            }

            void stop()
            {
                releaseHardware();
                if (m_state != DaqState::Idle)
                {
                    emitState(DaqState::Idle, QStringLiteral("DAQ device is idle"));
                }
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
                InputKind inputKind{InputKind::None};
                QString name;
                QStringList channels;
                quint64 nextSample{0};
                double sampleRateHz{0.0};
            };

            bool configureTiming(TaskHandle handle,
                                 const DaqTaskTiming& timing,
                                 QString& errorMessage)
            {
                const QByteArray clock = absoluteTerminal(m_device, timing.sampleClock).toLocal8Bit();
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
                    const QByteArray trigger = absoluteTerminal(m_device,
                                                                  timing.startTrigger).toLocal8Bit();
                    result = m_api.configureTrigger(
                        handle,
                        trigger.constData(),
                        timing.startEdge == DaqEdge::Rising ? kRising : kFalling);
                }
                if (result < 0)
                {
                    errorMessage = m_api.error(result);
                    return false;
                }
                return true;
            }

            bool createPulseTask(const DaqPulseTaskConfig& config,
                                 int index,
                                 QString& errorMessage)
            {
                TaskHandle handle = nullptr;
                const QString name = config.name.trimmed().isEmpty()
                                         ? QStringLiteral("Pulse Task %1").arg(index + 1)
                                         : config.name.trimmed();
                Int32 result = m_api.createTask(name.toLocal8Bit().constData(), &handle);
                const QByteArray counter = absoluteTerminal(m_device, config.counter).toLocal8Bit();
                const QByteArray output = absoluteTerminal(m_device, config.outputTerminal).toLocal8Bit();
                if (result >= 0 && !config.timebaseSource.trimmed().isEmpty())
                {
                    const QByteArray timebase = absoluteTerminal(m_device,
                                                                   config.timebaseSource).toLocal8Bit();
                    result = m_api.createPulseTicks(
                        handle,
                        counter.constData(),
                        nullptr,
                        timebase.constData(),
                        kLow,
                        config.initialDelayTicks,
                        config.lowTicks,
                        config.highTicks);
                }
                else if (result >= 0)
                {
                    result = m_api.createPulse(
                        handle,
                        counter.constData(),
                        nullptr,
                        kHertz,
                        kLow,
                        config.initialDelaySeconds,
                        config.frequencyHz,
                        config.dutyCycle);
                }
                if (result >= 0 && !config.outputTerminal.trimmed().isEmpty())
                {
                    result = m_api.setChannelString(handle,
                                                    counter.constData(),
                                                    output.constData());
                }
                if (result >= 0)
                {
                    result = m_api.configureImplicit(
                        handle,
                        kContinuousSamples,
                        1000);
                }
                if (result >= 0 && !config.startTrigger.trimmed().isEmpty())
                {
                    const QByteArray trigger = absoluteTerminal(m_device,
                                                                  config.startTrigger).toLocal8Bit();
                    result = m_api.configureTrigger(
                        handle,
                        trigger.constData(),
                        config.startEdge == DaqEdge::Rising ? kRising : kFalling);
                }
                if (result < 0)
                {
                    errorMessage = m_api.error(result);
                    if (handle)
                    {
                        m_api.clearTask(handle);
                    }
                    return false;
                }
                m_tasks.push_back({handle,
                                   InputKind::None,
                                   name,
                                   {},
                                   0,
                                   0.0});
                return true;
            }

            bool createAnalogTask(const DaqAnalogTaskConfig& config,
                                  int index,
                                  QString& errorMessage)
            {
                TaskHandle handle = nullptr;
                const QString name = config.name.trimmed().isEmpty()
                                         ? QStringLiteral("Analog Task %1").arg(index + 1)
                                         : config.name.trimmed();
                Int32 result = m_api.createTask(name.toLocal8Bit().constData(), &handle);
                const QByteArray channels = config.channels.join(QLatin1Char(',')).toLocal8Bit();
                if (result >= 0 && config.direction == DaqTaskDirection::Input)
                {
                    result = m_api.createAiVoltage(
                        handle,
                        channels.constData(),
                        "",
                        kDefaultTerminalConfiguration,
                        config.minimumVolts,
                        config.maximumVolts,
                        kVolts,
                        nullptr);
                }
                else if (result >= 0)
                {
                    result = m_api.createAoVoltage(
                        handle,
                        channels.constData(),
                        "",
                        config.minimumVolts,
                        config.maximumVolts,
                        kVolts,
                        nullptr);
                }
                if (result >= 0 && !configureTiming(handle, config.timing, errorMessage))
                {
                    result = -1;
                }
                if (result >= 0 && config.direction == DaqTaskDirection::Output)
                {
                    Int32 written = 0;
                    result = m_api.writeAnalog(
                        handle,
                        static_cast<Int32>(config.timing.samplesPerChannel),
                        0,
                        10.0,
                        kGroupByScanNumber,
                        config.outputSamplesByScan.constData(),
                        &written,
                        nullptr);
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
                m_tasks.push_back({handle,
                                   config.direction == DaqTaskDirection::Input
                                       ? InputKind::Analog
                                       : InputKind::None,
                                   name,
                                   config.channels,
                                   0,
                                   config.timing.sampleRateHz});
                return true;
            }

            bool createDigitalTask(const DaqDigitalTaskConfig& config,
                                   int index,
                                   QString& errorMessage)
            {
                TaskHandle handle = nullptr;
                const QString name = config.name.trimmed().isEmpty()
                                         ? QStringLiteral("Digital Task %1").arg(index + 1)
                                         : config.name.trimmed();
                Int32 result = m_api.createTask(name.toLocal8Bit().constData(), &handle);
                const QByteArray lines = config.lines.join(QLatin1Char(',')).toLocal8Bit();
                if (result >= 0)
                {
                    result = config.direction == DaqTaskDirection::Input
                        ? m_api.createDi(
                            handle,
                            lines.constData(),
                            "",
                            kChannelForAllLines)
                        : m_api.createDo(
                        handle,
                        lines.constData(),
                        "",
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
                        handle,
                        static_cast<Int32>(config.timing.samplesPerChannel),
                        0,
                        10.0,
                        kGroupByScanNumber,
                        config.outputSamplesByScan.constData(),
                        &written,
                        nullptr);
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
                m_tasks.push_back({handle,
                                   config.direction == DaqTaskDirection::Input
                                       ? InputKind::Digital
                                       : InputKind::None,
                                   name,
                                   config.lines,
                                   0,
                                   config.timing.sampleRateHz});
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
                        chunk.nominalSampleRateHz = task.sampleRateHz;
                        Int32 samplesRead = 0;
                        if (task.inputKind == InputKind::Analog)
                        {
                            chunk.analogSamplesByScan.resize(
                                static_cast<qsizetype>(samples * task.channels.size()));
                            result = m_api.readAnalog(
                                task.handle,
                                static_cast<Int32>(samples),
                                0.0,
                                kGroupByScanNumber,
                                chunk.analogSamplesByScan.data(),
                                static_cast<UInt32>(chunk.analogSamplesByScan.size()),
                                &samplesRead,
                                nullptr);
                            chunk.analogSamplesByScan.resize(
                                static_cast<qsizetype>(samplesRead * task.channels.size()));
                        }
                        else
                        {
                            chunk.digitalSamplesByScan.resize(static_cast<qsizetype>(samples));
                            result = m_api.readDigital(
                                task.handle,
                                static_cast<Int32>(samples),
                                0.0,
                                kGroupByScanNumber,
                                chunk.digitalSamplesByScan.data(),
                                static_cast<UInt32>(chunk.digitalSamplesByScan.size()),
                                &samplesRead,
                                nullptr);
                            chunk.digitalSamplesByScan.resize(samplesRead);
                        }
                        if (result < 0)
                        {
                            reportReadError(m_api.error(result));
                            return;
                        }
                        if (samplesRead > 0)
                        {
                            task.nextSample += static_cast<quint64>(samplesRead);
                            m_input(chunk);
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
                    m_api.disconnectTerminal(it->first.constData(), it->second.constData());
                }
                m_routes.clear();
            }

            // Runs on the reader thread, which cannot join itself, so the tasks are cleared on the next start or stop
            void reportReadError(const QString& message)
            {
                m_reading.store(false);
                emitState(DaqState::Error, message);
            }

            bool fail(const QString& message, QString* errorMessage)
            {
                releaseHardware();
                if (errorMessage)
                {
                    *errorMessage = message;
                }
                emitState(DaqState::Error, message);
                return false;
            }

            void emitState(DaqState state, const QString& message)
            {
                m_state = state;
                m_stateSink(state, message);
            }

            QString m_device;
            InputSink m_input;
            StateSink m_stateSink;
            DaqmxApi m_api;
            std::vector<ActiveTask> m_tasks;
            std::vector<std::pair<QByteArray, QByteArray>> m_routes;
            std::atomic_bool m_reading{false};
            std::thread m_reader;
            std::atomic<DaqState> m_state{DaqState::Idle};
        };

        SignalParameterDescriptor voltsParameter(const QString& key,
                                                 const QString& name,
                                                 double value)
        {
            SignalParameterDescriptor parameter;
            parameter.key = key;
            parameter.name = name;
            parameter.type = SignalParameterType::Real;
            parameter.defaultValue = value;
            parameter.hasRange = true;
            parameter.minimum = -10.0;
            parameter.maximum = 10.0;
            parameter.suffix = QStringLiteral(" V");
            return parameter;
        }

        // The analog inputs of one device that run as signal sources. A device runs one analog
        // input task at a time, so all of them are read by one task on the sample clock of the
        // first. Adding or removing a channel rebuilds the task, and the samples keep counting
        // from the first start, so the channels share one time axis and a rebuild leaves a gap.
        class AiTaskGroup final
        {
        public:
            using ChunkSink = std::function<void(const TimeSeriesChunk&)>;
            using StateSink = std::function<void(const QString&, SignalSourceState, const QString&)>;

            AiTaskGroup(QString device, ChunkSink chunks, StateSink state)
                : m_device(std::move(device))
                  , m_chunks(std::move(chunks))
                  , m_stateSink(std::move(state))
            {
            }

            ~AiTaskGroup()
            {
                stopReader();
            }

            bool add(const QString& sourceId,
                     const QString& channel,
                     const SignalAcquisitionConfig& config,
                     QString* errorMessage)
            {
                // load resolves the function pointers again, so the reader that calls them stops first
                stopReader();
                QString loadError;
                if (!m_api.load(true, &loadError))
                {
                    return fail(sourceId, loadError, errorMessage);
                }
                quint64 offset = 0;
                {
                    std::lock_guard lock(m_mutex);
                    for (const Member& member : m_members)
                    {
                        if (member.sourceId == sourceId)
                        {
                            restartLocked();
                            return fail(sourceId, QStringLiteral("%1 is already acquiring").arg(channel),
                                        errorMessage);
                        }
                    }
                    if (m_members.empty())
                    {
                        m_interval = config.sampleIntervalSeconds;
                        m_origin = std::chrono::steady_clock::now();
                    }
                    else if (std::abs(config.sampleIntervalSeconds - m_interval) > 1e-9 * m_interval)
                    {
                        restartLocked();
                        return fail(sourceId,
                                    QStringLiteral("%1 already samples its analog inputs every %2 s, so %3 "
                                                   "has to use the same interval")
                                        .arg(m_device).arg(m_interval).arg(channel),
                                    errorMessage);
                    }
                    offset = elapsedSamples();
                    Member member;
                    member.sourceId = sourceId;
                    member.channel = channel;
                    member.minimum = config.sourceSettings.value(QStringLiteral("minimumVolts"), -10.0).toDouble();
                    member.maximum = config.sourceSettings.value(QStringLiteral("maximumVolts"), 10.0).toDouble();
                    member.endSample = offset + static_cast<quint64>(
                        std::max(0.0, std::ceil(config.durationMs / 1000.0 / m_interval)));
                    m_members.push_back(member);
                }
                m_stateSink(sourceId, SignalSourceState::Starting, QStringLiteral("Configuring %1").arg(channel));
                QString error;
                {
                    std::lock_guard lock(m_mutex);
                    error = restartLocked();
                }
                if (!error.isEmpty())
                {
                    if (errorMessage)
                    {
                        *errorMessage = error;
                    }
                    return false;
                }
                m_stateSink(sourceId, SignalSourceState::Running, QStringLiteral("%1 is acquiring").arg(channel));
                return true;
            }

            void remove(const QString& sourceId)
            {
                stopReader();
                QString channel;
                {
                    std::lock_guard lock(m_mutex);
                    const auto found = std::find_if(m_members.begin(), m_members.end(),
                                                    [&sourceId](const Member& member)
                                                    {
                                                        return member.sourceId == sourceId;
                                                    });
                    if (found != m_members.end())
                    {
                        channel = found->channel;
                        m_members.erase(found);
                    }
                    restartLocked();
                }
                if (!channel.isEmpty())
                {
                    m_stateSink(sourceId, SignalSourceState::Idle, QStringLiteral("%1 stopped").arg(channel));
                }
            }

        private:
            struct Member
            {
                QString sourceId;
                QString channel;
                double minimum{-10.0};
                double maximum{10.0};
                quint64 endSample{0};
            };

            quint64 elapsedSamples() const
            {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - m_origin).count();
                return static_cast<quint64>(std::llround(elapsed / m_interval));
            }

            // Builds and starts the task for the current members; on failure every member ends in
            // the error, since the device then reads none of them
            QString restartLocked()
            {
                if (m_members.empty())
                {
                    return {};
                }
                const double rate = 1.0 / m_interval;
                // The buffer of a continuous task holds two seconds, as the stream ring does
                const auto bufferSamples = static_cast<UInt64>(std::max(1000.0, 2.0 * rate));
                TaskHandle handle = nullptr;
                Int32 result = m_api.createTask("", &handle);
                for (const Member& member : m_members)
                {
                    if (result < 0)
                    {
                        break;
                    }
                    const QByteArray channel = member.channel.toLocal8Bit();
                    result = m_api.createAiVoltage(handle, channel.constData(), "",
                                                   kDefaultTerminalConfiguration,
                                                   member.minimum, member.maximum, kVolts, nullptr);
                }
                if (result >= 0)
                {
                    result = m_api.configureSampleClock(handle, "", rate, kRising,
                                                        kContinuousSamples, bufferSamples);
                }
                if (result >= 0)
                {
                    result = m_api.startTask(handle);
                }
                if (result < 0)
                {
                    const QString message = m_api.error(result);
                    if (handle)
                    {
                        m_api.clearTask(handle);
                    }
                    for (const Member& member : m_members)
                    {
                        m_stateSink(member.sourceId, SignalSourceState::Error, message);
                    }
                    m_members.clear();
                    return message;
                }
                const quint64 offset = elapsedSamples();
                m_reading.store(true);
                m_reader = std::thread([this, handle, members = m_members, offset]()
                {
                    read(handle, members, offset);
                });
                return {};
            }

            void stopReader()
            {
                m_reading.store(false);
                if (m_reader.joinable())
                {
                    m_reader.join();
                }
            }

            // Reports only how members end by themselves, finished or failed; a member that is
            // stopped or a task that is rebuilt is reported by the caller
            void read(TaskHandle handle, std::vector<Member> members, quint64 offset)
            {
                constexpr UInt32 kSamplesPerRead = 65536;
                const auto channelCount = static_cast<qsizetype>(members.size());
                std::vector<bool> finished(members.size(), false);
                qsizetype running = channelCount;
                QVector<double> interleaved;
                quint64 nextSample = offset;
                QString error;
                while (m_reading.load() && running > 0)
                {
                    UInt32 available = 0;
                    Int32 result = m_api.availableSamples(handle, &available);
                    if (result >= 0 && available > 0)
                    {
                        const UInt32 wanted = std::min(available, kSamplesPerRead);
                        interleaved.resize(static_cast<qsizetype>(wanted) * channelCount);
                        Int32 samplesRead = 0;
                        result = m_api.readAnalog(handle, static_cast<Int32>(wanted), 0.0,
                                                  kGroupByScanNumber, interleaved.data(),
                                                  static_cast<UInt32>(interleaved.size()),
                                                  &samplesRead, nullptr);
                        if (result >= 0 && samplesRead > 0)
                        {
                            publish(members, finished, running, interleaved, nextSample,
                                    static_cast<quint64>(samplesRead));
                            nextSample += static_cast<quint64>(samplesRead);
                        }
                    }
                    if (result < 0)
                    {
                        error = m_api.error(result);
                        break;
                    }
                    if (available == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                m_api.stopTask(handle);
                m_api.clearTask(handle);
                if (error.isEmpty())
                {
                    return;
                }
                std::lock_guard lock(m_mutex);
                for (std::size_t index = 0; index < members.size(); ++index)
                {
                    if (!finished[index])
                    {
                        dropMemberLocked(members[index].sourceId);
                        m_stateSink(members[index].sourceId, SignalSourceState::Error, error);
                    }
                }
            }

            void publish(const std::vector<Member>& members,
                         std::vector<bool>& finished,
                         qsizetype& running,
                         const QVector<double>& interleaved,
                         quint64 firstSample,
                         quint64 count)
            {
                const auto channelCount = static_cast<qsizetype>(members.size());
                for (qsizetype channel = 0; channel < channelCount; ++channel)
                {
                    const Member& member = members[static_cast<std::size_t>(channel)];
                    if (finished[static_cast<std::size_t>(channel)])
                    {
                        continue;
                    }
                    const quint64 usable = std::min(count, member.endSample > firstSample
                                                               ? member.endSample - firstSample
                                                               : 0);
                    if (usable > 0)
                    {
                        TimeSeriesChunk chunk;
                        chunk.sourceId = member.sourceId;
                        chunk.quantity = QStringLiteral("Voltage");
                        chunk.unit = QStringLiteral("V");
                        chunk.startTimeSeconds = static_cast<double>(firstSample) * m_interval;
                        chunk.sampleIntervalSeconds = m_interval;
                        chunk.values.resize(static_cast<qsizetype>(usable));
                        for (qsizetype sample = 0; sample < chunk.values.size(); ++sample)
                        {
                            chunk.values[sample] = interleaved[sample * channelCount + channel];
                        }
                        m_chunks(chunk);
                    }
                    if (firstSample + count >= member.endSample)
                    {
                        finished[static_cast<std::size_t>(channel)] = true;
                        --running;
                        {
                            std::lock_guard lock(m_mutex);
                            dropMemberLocked(member.sourceId);
                        }
                        m_stateSink(member.sourceId, SignalSourceState::Idle,
                                    QStringLiteral("%1 finished").arg(member.channel));
                    }
                }
            }

            void dropMemberLocked(const QString& sourceId)
            {
                m_members.erase(std::remove_if(m_members.begin(), m_members.end(),
                                               [&sourceId](const Member& member)
                                               {
                                                   return member.sourceId == sourceId;
                                               }),
                                m_members.end());
            }

            bool fail(const QString& sourceId, const QString& message, QString* errorMessage)
            {
                if (errorMessage)
                {
                    *errorMessage = message;
                }
                m_stateSink(sourceId, SignalSourceState::Error, message);
                return false;
            }

            QString m_device;
            ChunkSink m_chunks;
            StateSink m_stateSink;
            DaqmxApi m_api;
            std::mutex m_mutex;
            std::vector<Member> m_members;
            double m_interval{0.0};
            std::chrono::steady_clock::time_point m_origin;
            std::atomic_bool m_reading{false};
            std::thread m_reader;
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

    QList<DaqDeviceDescriptor> findDevices(QString* errorMessage)
    {
        DaqmxApi api;
        if (!api.load(false, errorMessage))
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
            appendChannels(descriptor,
                           readDeviceString(api.getAiChannels, device),
                           DaqChannelType::AnalogInput);
            appendChannels(descriptor,
                           readDeviceString(api.getAoChannels, device),
                           DaqChannelType::AnalogOutput);
            appendChannels(descriptor,
                           readDeviceString(api.getDiLines, device),
                           DaqChannelType::DigitalInput);
            appendChannels(descriptor,
                           readDeviceString(api.getDoLines, device),
                           DaqChannelType::DigitalOutput);
            appendChannels(descriptor,
                           readDeviceString(api.getCiChannels, device),
                           DaqChannelType::CounterInput);
            appendChannels(descriptor,
                           readDeviceString(api.getCoChannels, device),
                           DaqChannelType::CounterOutput);
            descriptor.terminals = splitNames(
                readDeviceString(api.getTerminals, device).toLocal8Bit());
            result.append(std::move(descriptor));
        }
        return result;
    }


    // The DAQ devices for timed sessions, and each of their analog inputs as a signal source of
    // its own. The sources of one device run in one analog input task. A device runs one such task
    // at a time, so its sources and a session that reads its analog inputs do not run together,
    // and NI-DAQmx says so when the later one starts.
    class NIDaqmxProvider final : public HardwareProvider, public DaqProvider, public SignalSourceProvider
    {
    public:
        explicit NIDaqmxProvider(QList<DaqDeviceDescriptor> devices)
            : m_devices(std::move(devices))
        {
            for (const DaqDeviceDescriptor& device : m_devices)
            {
                for (const DaqChannelDescriptor& channel : device.channels)
                {
                    if (channel.type != DaqChannelType::AnalogInput)
                    {
                        continue;
                    }
                    SignalSourceDescriptor source;
                    source.id = QStringLiteral("ni:%1").arg(channel.physicalName);
                    source.name = QStringLiteral("%1 (%2)").arg(channel.physicalName, device.product);
                    source.provider = QStringLiteral("National Instruments");
                    source.quantity = QStringLiteral("Voltage");
                    source.unit = QStringLiteral("V");
                    source.streamType = SignalStreamType::TimeSeries;
                    source.parameters.append(voltsParameter(
                        QStringLiteral("minimumVolts"), QStringLiteral("Minimum"), -10.0));
                    source.parameters.append(voltsParameter(
                        QStringLiteral("maximumVolts"), QStringLiteral("Maximum"), 10.0));
                    m_sources.append(source);
                    m_sourceChannels.insert(source.id, channel.physicalName);
                    m_sourceDevices.insert(source.id, device.name);
                }
            }
        }


        HardwareProviderDescriptor descriptor() const override
        {
            return {QStringLiteral("scopeone.nidaqmx"), QStringLiteral("NI-DAQmx"), QStringLiteral("1.0.0")};
        }

        // The DAQ devices are listed as DAQ devices, not as devices with properties
        QList<HardwareDeviceDescriptor> devices() const override
        {
            return {};
        }

        QList<DaqDeviceDescriptor> daqDevices() const override
        {
            return m_devices;
        }

        void setDaqSinks(InputSink input, DaqProvider::StateSink state) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_inputSink = std::move(input);
            m_stateSink = std::move(state);
        }

        bool startDaq(const DaqSessionConfig& config, QString* errorMessage) override
        {
            NIDaqmxController* const device = controller(config.deviceId);
            if (!device)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unknown DAQ device %1").arg(config.deviceId);
                }
                return false;
            }
            return device->start(config, errorMessage);
        }

        void stopDaq(const QString& deviceId) override
        {
            if (NIDaqmxController* const device = m_controllers.value(deviceId).get())
            {
                device->stop();
            }
        }

        QList<SignalSourceDescriptor> signalSources() const override
        {
            return m_sources;
        }

        void setSignalSinks(TimeSeriesSink timeSeries,
                            TimestampedEventSink,
                            SignalSourceProvider::StateSink state) override
        {
            QMutexLocker locker(&m_sinkMutex);
            m_timeSeriesSink = std::move(timeSeries);
            m_signalStateSink = std::move(state);
        }

        bool startSignal(const SignalAcquisitionConfig& config, QString* errorMessage) override
        {
            AiTaskGroup* const group = aiGroup(config.sourceId);
            if (!group)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unknown signal source %1").arg(config.sourceId);
                }
                return false;
            }
            return group->add(config.sourceId, m_sourceChannels.value(config.sourceId), config, errorMessage);
        }

        void stopSignal(const QString& sourceId) override
        {
            if (AiTaskGroup* const group = m_aiGroups.value(m_sourceDevices.value(sourceId)).get())
            {
                group->remove(sourceId);
            }
        }

    private:
        AiTaskGroup* aiGroup(const QString& sourceId)
        {
            const QString device = m_sourceDevices.value(sourceId);
            if (device.isEmpty())
            {
                return nullptr;
            }
            if (const auto existing = m_aiGroups.value(device))
            {
                return existing.get();
            }
            auto created = std::make_shared<AiTaskGroup>(
                device,
                [this](const TimeSeriesChunk& chunk)
                {
                    QMutexLocker locker(&m_sinkMutex);
                    if (m_timeSeriesSink) m_timeSeriesSink(chunk);
                },
                [this](const QString& id, SignalSourceState state, const QString& message)
                {
                    QMutexLocker locker(&m_sinkMutex);
                    if (m_signalStateSink) m_signalStateSink(id, state, message);
                });
            m_aiGroups.insert(device, created);
            return created.get();
        }

        NIDaqmxController* controller(const QString& deviceId)
        {
            if (const auto existing = m_controllers.value(deviceId))
            {
                return existing.get();
            }
            const bool known = std::any_of(m_devices.cbegin(), m_devices.cend(),
                                           [&deviceId](const DaqDeviceDescriptor& device)
                                           {
                                               return device.id == deviceId;
                                           });
            if (!known)
            {
                return nullptr;
            }
            auto created = std::make_shared<NIDaqmxController>(
                nativeDeviceId(deviceId),
                [this](const DaqInputChunk& chunk)
                {
                    QMutexLocker locker(&m_sinkMutex);
                    if (m_inputSink) m_inputSink(chunk);
                },
                [this, deviceId](DaqState state, const QString& message)
                {
                    QMutexLocker locker(&m_sinkMutex);
                    if (m_stateSink) m_stateSink(deviceId, state, message);
                });
            m_controllers.insert(deviceId, created);
            return created.get();
        }

        QList<DaqDeviceDescriptor> m_devices;
        QList<SignalSourceDescriptor> m_sources;
        QHash<QString, QString> m_sourceChannels;
        QHash<QString, QString> m_sourceDevices;
        QMutex m_sinkMutex;
        InputSink m_inputSink;
        DaqProvider::StateSink m_stateSink;
        TimeSeriesSink m_timeSeriesSink;
        SignalSourceProvider::StateSink m_signalStateSink;
        // Declared last so they are destroyed first: their readers call the sinks above until joined
        QHash<QString, std::shared_ptr<NIDaqmxController>> m_controllers;
        QHash<QString, std::shared_ptr<AiTaskGroup>> m_aiGroups;
    };

    class NIDaqmxDriver final : public QObject, public Driver
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
        Q_INTERFACES(scopeone::core::Driver)

    public:
        QString providerId() const override
        {
            return QStringLiteral("scopeone.nidaqmx");
        }

        // Fails when the NI-DAQmx runtime is not installed, so the driver can be turned off under Drivers and Plugins
        HardwareProviderPtr createProvider(const QJsonObject&, QString* errorMessage) override
        {
            QString error;
            QList<DaqDeviceDescriptor> devices = findDevices(&error);
            if (!error.isEmpty())
            {
                if (errorMessage) *errorMessage = error;
                return {};
            }
            if (errorMessage) errorMessage->clear();
            return std::make_shared<NIDaqmxProvider>(std::move(devices));
        }
    };
}

#include "NIDaqmxDriver.moc"
