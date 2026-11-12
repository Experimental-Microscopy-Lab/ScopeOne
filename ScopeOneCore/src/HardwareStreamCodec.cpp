#include "internal/HardwareStreamCodec.h"

#include <QJsonArray>

#include <cstring>
#include <type_traits>

namespace scopeone::core::internal::hardwarestream
{
    namespace
    {
        QJsonArray toJsonArray(const QStringList& values)
        {
            return QJsonArray::fromStringList(values);
        }

        QStringList stringList(const QJsonValue& value)
        {
            QStringList result;
            for (const QJsonValue& item : value.toArray())
            {
                result.append(item.toString());
            }
            return result;
        }

        QString encodeUInt64(quint64 value)
        {
            return QString::number(value);
        }

        quint64 decodeUInt64(const QJsonValue& value)
        {
            return value.toString().toULongLong();
        }

        // Waveforms can hold many samples, so they travel as the bytes of the array
        template <typename T>
        QString encodeSamples(const QVector<T>& samples)
        {
            return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(samples.constData()),
                                                  samples.size() * static_cast<qsizetype>(sizeof(T)))
                                           .toBase64());
        }

        template <typename T>
        QVector<T> decodeSamples(const QJsonValue& value)
        {
            const QByteArray bytes = QByteArray::fromBase64(value.toString().toLatin1());
            QVector<T> samples(bytes.size() / static_cast<qsizetype>(sizeof(T)));
            std::memcpy(samples.data(), bytes.constData(),
                        static_cast<std::size_t>(samples.size()) * sizeof(T));
            return samples;
        }

        QJsonObject toJson(const DaqTaskTiming& timing)
        {
            return {{QStringLiteral("sampleClock"), timing.sampleClock},
                    {QStringLiteral("sampleRateHz"), timing.sampleRateHz},
                    {QStringLiteral("sampleEdge"), static_cast<int>(timing.sampleEdge)},
                    {QStringLiteral("sampleMode"), static_cast<int>(timing.sampleMode)},
                    {QStringLiteral("samplesPerChannel"), encodeUInt64(timing.samplesPerChannel)},
                    {QStringLiteral("startTrigger"), timing.startTrigger},
                    {QStringLiteral("startEdge"), static_cast<int>(timing.startEdge)}};
        }

        DaqTaskTiming timingFromJson(const QJsonObject& object)
        {
            DaqTaskTiming timing;
            timing.sampleClock = object.value(QStringLiteral("sampleClock")).toString();
            timing.sampleRateHz = object.value(QStringLiteral("sampleRateHz")).toDouble();
            timing.sampleEdge = static_cast<DaqEdge>(object.value(QStringLiteral("sampleEdge")).toInt());
            timing.sampleMode = static_cast<DaqSampleMode>(object.value(QStringLiteral("sampleMode")).toInt());
            timing.samplesPerChannel = decodeUInt64(object.value(QStringLiteral("samplesPerChannel")));
            timing.startTrigger = object.value(QStringLiteral("startTrigger")).toString();
            timing.startEdge = static_cast<DaqEdge>(object.value(QStringLiteral("startEdge")).toInt());
            return timing;
        }

        class Output
        {
        public:
            template <typename T>
            void value(T item)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                m_bytes.append(reinterpret_cast<const char*>(&item), sizeof(T));
            }

            void string(const QString& text)
            {
                const QByteArray utf8 = text.toUtf8();
                value<quint32>(static_cast<quint32>(utf8.size()));
                m_bytes.append(utf8);
            }

            void strings(const QStringList& texts)
            {
                value<quint32>(static_cast<quint32>(texts.size()));
                for (const QString& text : texts)
                {
                    string(text);
                }
            }

            template <typename T>
            void array(const QVector<T>& items)
            {
                value<quint64>(static_cast<quint64>(items.size()));
                m_bytes.append(reinterpret_cast<const char*>(items.constData()),
                               items.size() * static_cast<qsizetype>(sizeof(T)));
            }

            QByteArray bytes() const { return m_bytes; }

        private:
            QByteArray m_bytes;
        };

        class Input
        {
        public:
            explicit Input(const QByteArray& bytes)
                : m_bytes(bytes)
            {
            }

            template <typename T>
            T value()
            {
                T item{};
                if (!take(&item, sizeof(T)))
                {
                    return {};
                }
                return item;
            }

            QString string()
            {
                const quint32 size = value<quint32>();
                if (!m_ok || size > remaining())
                {
                    m_ok = false;
                    return {};
                }
                const QString text = QString::fromUtf8(m_bytes.constData() + m_position,
                                                       static_cast<qsizetype>(size));
                m_position += size;
                return text;
            }

            QStringList strings()
            {
                const quint32 count = value<quint32>();
                QStringList texts;
                for (quint32 index = 0; m_ok && index < count; ++index)
                {
                    texts.append(string());
                }
                return texts;
            }

            template <typename T>
            QVector<T> array()
            {
                const quint64 count = value<quint64>();
                if (!m_ok || count > remaining() / sizeof(T))
                {
                    m_ok = false;
                    return {};
                }
                QVector<T> items(static_cast<qsizetype>(count));
                take(items.data(), static_cast<std::size_t>(count) * sizeof(T));
                return items;
            }

            bool ok() const { return m_ok; }

        private:
            quint64 remaining() const
            {
                return static_cast<quint64>(m_bytes.size() - m_position);
            }

            bool take(void* target, std::size_t size)
            {
                if (!m_ok || size > remaining())
                {
                    m_ok = false;
                    return false;
                }
                std::memcpy(target, m_bytes.constData() + m_position, size);
                m_position += static_cast<qsizetype>(size);
                return true;
            }

            const QByteArray& m_bytes;
            qsizetype m_position{0};
            bool m_ok{true};
        };
    }

    QJsonObject toJson(const SignalSourceDescriptor& descriptor)
    {
        QJsonArray parameters;
        for (const SignalParameterDescriptor& parameter : descriptor.parameters)
        {
            parameters.append(QJsonObject{
                {QStringLiteral("key"), parameter.key},
                {QStringLiteral("name"), parameter.name},
                {QStringLiteral("type"), static_cast<int>(parameter.type)},
                {QStringLiteral("defaultValue"), QJsonValue::fromVariant(parameter.defaultValue)},
                {QStringLiteral("hasRange"), parameter.hasRange},
                {QStringLiteral("minimum"), parameter.minimum},
                {QStringLiteral("maximum"), parameter.maximum},
                {QStringLiteral("suffix"), parameter.suffix},
                {QStringLiteral("choices"), QJsonArray::fromVariantList(parameter.choices)},
                {QStringLiteral("choiceNames"), toJsonArray(parameter.choiceNames)},
                {QStringLiteral("fileFilter"), parameter.fileFilter}});
        }
        return {{QStringLiteral("id"), descriptor.id},
                {QStringLiteral("name"), descriptor.name},
                {QStringLiteral("provider"), descriptor.provider},
                {QStringLiteral("quantity"), descriptor.quantity},
                {QStringLiteral("unit"), descriptor.unit},
                {QStringLiteral("streamType"), static_cast<int>(descriptor.streamType)},
                {QStringLiteral("parameters"), parameters}};
    }

    SignalSourceDescriptor signalSourceDescriptorFromJson(const QJsonObject& object)
    {
        SignalSourceDescriptor descriptor;
        descriptor.id = object.value(QStringLiteral("id")).toString();
        descriptor.name = object.value(QStringLiteral("name")).toString();
        descriptor.provider = object.value(QStringLiteral("provider")).toString();
        descriptor.quantity = object.value(QStringLiteral("quantity")).toString();
        descriptor.unit = object.value(QStringLiteral("unit")).toString();
        descriptor.streamType = static_cast<SignalStreamType>(object.value(QStringLiteral("streamType")).toInt());
        for (const QJsonValue& value : object.value(QStringLiteral("parameters")).toArray())
        {
            const QJsonObject item = value.toObject();
            SignalParameterDescriptor parameter;
            parameter.key = item.value(QStringLiteral("key")).toString();
            parameter.name = item.value(QStringLiteral("name")).toString();
            parameter.type = static_cast<SignalParameterType>(item.value(QStringLiteral("type")).toInt());
            parameter.defaultValue = item.value(QStringLiteral("defaultValue")).toVariant();
            parameter.hasRange = item.value(QStringLiteral("hasRange")).toBool();
            parameter.minimum = item.value(QStringLiteral("minimum")).toDouble();
            parameter.maximum = item.value(QStringLiteral("maximum")).toDouble();
            parameter.suffix = item.value(QStringLiteral("suffix")).toString();
            parameter.choices = item.value(QStringLiteral("choices")).toArray().toVariantList();
            parameter.choiceNames = stringList(item.value(QStringLiteral("choiceNames")));
            parameter.fileFilter = item.value(QStringLiteral("fileFilter")).toString();
            descriptor.parameters.append(parameter);
        }
        return descriptor;
    }

    QJsonObject toJson(const SignalAcquisitionConfig& config)
    {
        const ScanImageConfig& scan = config.scanImage;
        return {{QStringLiteral("sourceId"), config.sourceId},
                {QStringLiteral("sampleIntervalSeconds"), config.sampleIntervalSeconds},
                {QStringLiteral("durationMs"), config.durationMs},
                {QStringLiteral("publishTimestampedEvents"), config.publishTimestampedEvents},
                {QStringLiteral("scanImage"), QJsonObject{
                     {QStringLiteral("enabled"), scan.enabled},
                     {QStringLiteral("width"), scan.width},
                     {QStringLiteral("height"), scan.height},
                     {QStringLiteral("gain"), static_cast<qint64>(scan.gain)},
                     {QStringLiteral("averageFrames"), scan.averageFrames},
                     {QStringLiteral("frameStartMarker"), static_cast<qint64>(scan.frameStartMarker)},
                     {QStringLiteral("lineMarker"), static_cast<qint64>(scan.lineMarker)},
                     {QStringLiteral("frameEndMarker"), static_cast<qint64>(scan.frameEndMarker)},
                     {QStringLiteral("serpentine"), scan.serpentine},
                     {QStringLiteral("mirrorHorizontal"), scan.mirrorHorizontal}}},
                {QStringLiteral("sourceSettings"), QJsonObject::fromVariantMap(config.sourceSettings)}};
    }

    SignalAcquisitionConfig signalAcquisitionConfigFromJson(const QJsonObject& object)
    {
        SignalAcquisitionConfig config;
        config.sourceId = object.value(QStringLiteral("sourceId")).toString();
        config.sampleIntervalSeconds = object.value(QStringLiteral("sampleIntervalSeconds")).toDouble();
        config.durationMs = object.value(QStringLiteral("durationMs")).toInt();
        config.publishTimestampedEvents = object.value(QStringLiteral("publishTimestampedEvents")).toBool();
        const QJsonObject scan = object.value(QStringLiteral("scanImage")).toObject();
        config.scanImage.enabled = scan.value(QStringLiteral("enabled")).toBool();
        config.scanImage.width = scan.value(QStringLiteral("width")).toInt();
        config.scanImage.height = scan.value(QStringLiteral("height")).toInt();
        config.scanImage.gain = static_cast<quint32>(scan.value(QStringLiteral("gain")).toInteger());
        config.scanImage.averageFrames = scan.value(QStringLiteral("averageFrames")).toInt();
        config.scanImage.frameStartMarker =
            static_cast<quint32>(scan.value(QStringLiteral("frameStartMarker")).toInteger());
        config.scanImage.lineMarker = static_cast<quint32>(scan.value(QStringLiteral("lineMarker")).toInteger());
        config.scanImage.frameEndMarker =
            static_cast<quint32>(scan.value(QStringLiteral("frameEndMarker")).toInteger());
        config.scanImage.serpentine = scan.value(QStringLiteral("serpentine")).toBool();
        config.scanImage.mirrorHorizontal = scan.value(QStringLiteral("mirrorHorizontal")).toBool();
        config.sourceSettings = object.value(QStringLiteral("sourceSettings")).toObject().toVariantMap();
        return config;
    }

    QJsonObject toJson(const DaqDeviceDescriptor& descriptor)
    {
        QJsonArray channels;
        for (const DaqChannelDescriptor& channel : descriptor.channels)
        {
            channels.append(QJsonObject{{QStringLiteral("physicalName"), channel.physicalName},
                                        {QStringLiteral("type"), static_cast<int>(channel.type)}});
        }
        return {{QStringLiteral("id"), descriptor.id},
                {QStringLiteral("name"), descriptor.name},
                {QStringLiteral("provider"), descriptor.provider},
                {QStringLiteral("product"), descriptor.product},
                {QStringLiteral("channels"), channels},
                {QStringLiteral("terminals"), toJsonArray(descriptor.terminals)}};
    }

    DaqDeviceDescriptor daqDeviceDescriptorFromJson(const QJsonObject& object)
    {
        DaqDeviceDescriptor descriptor;
        descriptor.id = object.value(QStringLiteral("id")).toString();
        descriptor.name = object.value(QStringLiteral("name")).toString();
        descriptor.provider = object.value(QStringLiteral("provider")).toString();
        descriptor.product = object.value(QStringLiteral("product")).toString();
        for (const QJsonValue& value : object.value(QStringLiteral("channels")).toArray())
        {
            const QJsonObject channel = value.toObject();
            descriptor.channels.append({channel.value(QStringLiteral("physicalName")).toString(),
                                        static_cast<DaqChannelType>(channel.value(QStringLiteral("type")).toInt())});
        }
        descriptor.terminals = stringList(object.value(QStringLiteral("terminals")));
        return descriptor;
    }

    QJsonObject toJson(const DaqSessionConfig& config)
    {
        QJsonArray pulseTasks;
        for (const DaqPulseTaskConfig& task : config.pulseTasks)
        {
            pulseTasks.append(QJsonObject{
                {QStringLiteral("name"), task.name},
                {QStringLiteral("counter"), task.counter},
                {QStringLiteral("outputTerminal"), task.outputTerminal},
                {QStringLiteral("frequencyHz"), task.frequencyHz},
                {QStringLiteral("dutyCycle"), task.dutyCycle},
                {QStringLiteral("initialDelaySeconds"), task.initialDelaySeconds},
                {QStringLiteral("startTrigger"), task.startTrigger},
                {QStringLiteral("startEdge"), static_cast<int>(task.startEdge)},
                {QStringLiteral("timebaseSource"), task.timebaseSource},
                {QStringLiteral("initialDelayTicks"), static_cast<qint64>(task.initialDelayTicks)},
                {QStringLiteral("lowTicks"), static_cast<qint64>(task.lowTicks)},
                {QStringLiteral("highTicks"), static_cast<qint64>(task.highTicks)}});
        }
        QJsonArray analogTasks;
        for (const DaqAnalogTaskConfig& task : config.analogTasks)
        {
            analogTasks.append(QJsonObject{
                {QStringLiteral("name"), task.name},
                {QStringLiteral("direction"), static_cast<int>(task.direction)},
                {QStringLiteral("channels"), toJsonArray(task.channels)},
                {QStringLiteral("minimumVolts"), task.minimumVolts},
                {QStringLiteral("maximumVolts"), task.maximumVolts},
                {QStringLiteral("timing"), toJson(task.timing)},
                {QStringLiteral("outputSamplesByScan"), encodeSamples(task.outputSamplesByScan)}});
        }
        QJsonArray digitalTasks;
        for (const DaqDigitalTaskConfig& task : config.digitalTasks)
        {
            digitalTasks.append(QJsonObject{
                {QStringLiteral("name"), task.name},
                {QStringLiteral("direction"), static_cast<int>(task.direction)},
                {QStringLiteral("lines"), toJsonArray(task.lines)},
                {QStringLiteral("timing"), toJson(task.timing)},
                {QStringLiteral("outputSamplesByScan"), encodeSamples(task.outputSamplesByScan)}});
        }
        QJsonArray routes;
        for (const DaqTerminalRoute& route : config.routes)
        {
            routes.append(QJsonObject{{QStringLiteral("source"), route.source},
                                      {QStringLiteral("destination"), route.destination},
                                      {QStringLiteral("inverted"), route.inverted}});
        }
        return {{QStringLiteral("deviceId"), config.deviceId},
                {QStringLiteral("pulseTasks"), pulseTasks},
                {QStringLiteral("analogTasks"), analogTasks},
                {QStringLiteral("digitalTasks"), digitalTasks},
                {QStringLiteral("routes"), routes}};
    }

    DaqSessionConfig daqSessionConfigFromJson(const QJsonObject& object)
    {
        DaqSessionConfig config;
        config.deviceId = object.value(QStringLiteral("deviceId")).toString();
        for (const QJsonValue& value : object.value(QStringLiteral("pulseTasks")).toArray())
        {
            const QJsonObject item = value.toObject();
            DaqPulseTaskConfig task;
            task.name = item.value(QStringLiteral("name")).toString();
            task.counter = item.value(QStringLiteral("counter")).toString();
            task.outputTerminal = item.value(QStringLiteral("outputTerminal")).toString();
            task.frequencyHz = item.value(QStringLiteral("frequencyHz")).toDouble();
            task.dutyCycle = item.value(QStringLiteral("dutyCycle")).toDouble();
            task.initialDelaySeconds = item.value(QStringLiteral("initialDelaySeconds")).toDouble();
            task.startTrigger = item.value(QStringLiteral("startTrigger")).toString();
            task.startEdge = static_cast<DaqEdge>(item.value(QStringLiteral("startEdge")).toInt());
            task.timebaseSource = item.value(QStringLiteral("timebaseSource")).toString();
            task.initialDelayTicks = static_cast<quint32>(item.value(QStringLiteral("initialDelayTicks")).toInteger());
            task.lowTicks = static_cast<quint32>(item.value(QStringLiteral("lowTicks")).toInteger());
            task.highTicks = static_cast<quint32>(item.value(QStringLiteral("highTicks")).toInteger());
            config.pulseTasks.append(task);
        }
        for (const QJsonValue& value : object.value(QStringLiteral("analogTasks")).toArray())
        {
            const QJsonObject item = value.toObject();
            DaqAnalogTaskConfig task;
            task.name = item.value(QStringLiteral("name")).toString();
            task.direction = static_cast<DaqTaskDirection>(item.value(QStringLiteral("direction")).toInt());
            task.channels = stringList(item.value(QStringLiteral("channels")));
            task.minimumVolts = item.value(QStringLiteral("minimumVolts")).toDouble();
            task.maximumVolts = item.value(QStringLiteral("maximumVolts")).toDouble();
            task.timing = timingFromJson(item.value(QStringLiteral("timing")).toObject());
            task.outputSamplesByScan = decodeSamples<double>(item.value(QStringLiteral("outputSamplesByScan")));
            config.analogTasks.append(task);
        }
        for (const QJsonValue& value : object.value(QStringLiteral("digitalTasks")).toArray())
        {
            const QJsonObject item = value.toObject();
            DaqDigitalTaskConfig task;
            task.name = item.value(QStringLiteral("name")).toString();
            task.direction = static_cast<DaqTaskDirection>(item.value(QStringLiteral("direction")).toInt());
            task.lines = stringList(item.value(QStringLiteral("lines")));
            task.timing = timingFromJson(item.value(QStringLiteral("timing")).toObject());
            task.outputSamplesByScan = decodeSamples<quint32>(item.value(QStringLiteral("outputSamplesByScan")));
            config.digitalTasks.append(task);
        }
        for (const QJsonValue& value : object.value(QStringLiteral("routes")).toArray())
        {
            const QJsonObject item = value.toObject();
            config.routes.append({item.value(QStringLiteral("source")).toString(),
                                  item.value(QStringLiteral("destination")).toString(),
                                  item.value(QStringLiteral("inverted")).toBool()});
        }
        return config;
    }

    QByteArray encode(const TimeSeriesChunk& chunk)
    {
        Output output;
        output.string(chunk.sourceId);
        output.string(chunk.quantity);
        output.string(chunk.unit);
        output.value(chunk.startTimeSeconds);
        output.value(chunk.sampleIntervalSeconds);
        output.value(chunk.totalInputEvents);
        output.value(chunk.totalMarkers);
        output.array(chunk.values);
        QVector<double> markerTimes;
        QVector<quint32> markerCodes;
        for (const SignalMarker& marker : chunk.markers)
        {
            markerTimes.append(marker.timeSeconds);
            markerCodes.append(marker.code);
        }
        output.array(markerTimes);
        output.array(markerCodes);
        return output.bytes();
    }

    QByteArray encode(const TimestampedEventChunk& chunk)
    {
        Output output;
        output.string(chunk.sourceId);
        output.value(chunk.tickPeriodSeconds);
        output.array(chunk.eventTicks);
        output.array(chunk.eventCodes);
        output.array(chunk.markerTicks);
        output.array(chunk.markerCodes);
        return output.bytes();
    }

    QByteArray encode(const DaqInputChunk& chunk)
    {
        Output output;
        output.string(chunk.deviceId);
        output.string(chunk.taskName);
        output.strings(chunk.channels);
        output.value(chunk.firstSample);
        output.value(chunk.nominalSampleRateHz);
        output.array(chunk.analogSamplesByScan);
        output.array(chunk.digitalSamplesByScan);
        return output.bytes();
    }

    bool decode(const QByteArray& payload, TimeSeriesChunk& chunk)
    {
        Input input(payload);
        chunk.sourceId = input.string();
        chunk.quantity = input.string();
        chunk.unit = input.string();
        chunk.startTimeSeconds = input.value<double>();
        chunk.sampleIntervalSeconds = input.value<double>();
        chunk.totalInputEvents = input.value<quint64>();
        chunk.totalMarkers = input.value<quint64>();
        chunk.values = input.array<double>();
        const QVector<double> markerTimes = input.array<double>();
        const QVector<quint32> markerCodes = input.array<quint32>();
        if (!input.ok() || markerTimes.size() != markerCodes.size())
        {
            return false;
        }
        chunk.markers.clear();
        for (qsizetype index = 0; index < markerTimes.size(); ++index)
        {
            chunk.markers.append({markerTimes[index], markerCodes[index]});
        }
        return true;
    }

    bool decode(const QByteArray& payload, TimestampedEventChunk& chunk)
    {
        Input input(payload);
        chunk.sourceId = input.string();
        chunk.tickPeriodSeconds = input.value<double>();
        chunk.eventTicks = input.array<quint64>();
        chunk.eventCodes = input.array<quint32>();
        chunk.markerTicks = input.array<quint64>();
        chunk.markerCodes = input.array<quint32>();
        return input.ok();
    }

    bool decode(const QByteArray& payload, DaqInputChunk& chunk)
    {
        Input input(payload);
        chunk.deviceId = input.string();
        chunk.taskName = input.string();
        chunk.channels = input.strings();
        chunk.firstSample = input.value<quint64>();
        chunk.nominalSampleRateHz = input.value<double>();
        chunk.analogSamplesByScan = input.array<double>();
        chunk.digitalSamplesByScan = input.array<quint32>();
        return input.ok();
    }
}
