#include "PtuFilePlugin.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace scopeone::plugins
{
    using namespace scopeone::core;

    namespace
    {
        constexpr auto kSourceId = "ptu:file";
        constexpr qsizetype kTagSize = 48;
        constexpr qsizetype kRecordsPerChunk = 65536;

        constexpr quint32 kTagEmpty = 0xffff0008U;
        constexpr quint32 kTagBool = 0x00000008U;
        constexpr quint32 kTagInt = 0x10000008U;
        constexpr quint32 kTagBitSet = 0x11000008U;
        constexpr quint32 kTagColor = 0x12000008U;
        constexpr quint32 kTagFloat = 0x20000008U;
        constexpr quint32 kTagDateTime = 0x21000008U;
        constexpr quint32 kTagFloatArray = 0x2001ffffU;
        constexpr quint32 kTagAnsiString = 0x4001ffffU;
        constexpr quint32 kTagWideString = 0x4002ffffU;
        constexpr quint32 kTagBinaryBlob = 0xffffffffU;

        constexpr quint32 kPicoHarpT3 = 0x00010303U;
        constexpr quint32 kPicoHarpT2 = 0x00010203U;
        constexpr quint32 kHydraHarpT3 = 0x00010304U;
        constexpr quint32 kHydraHarpT2 = 0x00010204U;
        constexpr quint32 kHydraHarp2T3 = 0x01010304U;
        constexpr quint32 kHydraHarp2T2 = 0x01010204U;
        constexpr quint32 kTimeHarp260NT3 = 0x00010305U;
        constexpr quint32 kTimeHarp260NT2 = 0x00010205U;
        constexpr quint32 kTimeHarp260PT3 = 0x00010306U;
        constexpr quint32 kTimeHarp260PT2 = 0x00010206U;
        constexpr quint32 kGenericT3 = 0x00010307U;
        constexpr quint32 kGenericT2 = 0x00010207U;

        enum class Decoder
        {
            PicoT2,
            PicoT3,
            HydraT2V1,
            HydraT3V1,
            HydraT2V2,
            HydraT3V2
        };

        struct PtuHeader
        {
            quint64 recordCount{0};
            quint32 recordType{0};
            double globalResolutionSeconds{0.0};
            Decoder decoder{Decoder::PicoT2};
        };

        struct PtuSettings
        {
            QString filePath;
            int detectorChannel{1};
            double sampleIntervalSeconds{0.01};
            bool publishEvents{false};
        };

        QString tagIdentifier(const QByteArray& bytes)
        {
            const int terminator = bytes.indexOf('\0');
            return QString::fromLatin1(terminator >= 0 ? bytes.first(terminator) : bytes);
        }

        double tagDouble(qint64 value)
        {
            const quint64 bits = static_cast<quint64>(value);
            double result = 0.0;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

        bool decoderForRecordType(quint32 recordType, Decoder& decoder)
        {
            switch (recordType)
            {
            case kPicoHarpT2:
                decoder = Decoder::PicoT2;
                return true;
            case kPicoHarpT3:
                decoder = Decoder::PicoT3;
                return true;
            case kHydraHarpT2:
                decoder = Decoder::HydraT2V1;
                return true;
            case kHydraHarpT3:
                decoder = Decoder::HydraT3V1;
                return true;
            case kHydraHarp2T2:
            case kTimeHarp260NT2:
            case kTimeHarp260PT2:
            case kGenericT2:
                decoder = Decoder::HydraT2V2;
                return true;
            case kHydraHarp2T3:
            case kTimeHarp260NT3:
            case kTimeHarp260PT3:
            case kGenericT3:
                decoder = Decoder::HydraT3V2;
                return true;
            default:
                return false;
            }
        }

        bool skipTagPayload(QFile& file, qint64 byteCount, QString& errorMessage)
        {
            if (byteCount < 0 || byteCount > file.size() - file.pos()
                || !file.seek(file.pos() + byteCount))
            {
                errorMessage = QStringLiteral("Invalid PTU tag payload length");
                return false;
            }
            return true;
        }

        bool readHeader(QFile& file, PtuHeader& header, QString& errorMessage)
        {
            const QByteArray magic = file.read(8);
            const QByteArray version = file.read(8);
            if (magic.size() != 8 || version.size() != 8
                || !magic.startsWith("PQTTTR"))
            {
                errorMessage = QStringLiteral("The selected file is not a PTU file");
                return false;
            }

            qint64 recordCount = -1;
            qint64 recordType = -1;
            double globalResolution = 0.0;
            while (true)
            {
                const QByteArray rawTag = file.read(kTagSize);
                if (rawTag.size() != kTagSize)
                {
                    errorMessage = QStringLiteral("Incomplete PTU header");
                    return false;
                }
                const auto* bytes = reinterpret_cast<const uchar*>(rawTag.constData());
                const QString identifier = tagIdentifier(rawTag.first(32));
                const quint32 type = qFromLittleEndian<quint32>(bytes + 36);
                const qint64 value = qFromLittleEndian<qint64>(bytes + 40);

                if (identifier == QStringLiteral("TTResult_NumberOfRecords"))
                {
                    recordCount = value;
                }
                else if (identifier == QStringLiteral("TTResultFormat_TTTRRecType"))
                {
                    recordType = value;
                }
                else if (identifier == QStringLiteral("MeasDesc_GlobalResolution"))
                {
                    globalResolution = tagDouble(value);
                }

                if (type == kTagFloatArray || type == kTagAnsiString
                    || type == kTagWideString || type == kTagBinaryBlob)
                {
                    if (!skipTagPayload(file, value, errorMessage))
                    {
                        return false;
                    }
                }
                else if (type != kTagEmpty && type != kTagBool && type != kTagInt
                         && type != kTagBitSet && type != kTagColor
                         && type != kTagFloat && type != kTagDateTime)
                {
                    errorMessage = QStringLiteral("Unsupported PTU tag type 0x%1")
                                       .arg(type, 8, 16, QLatin1Char('0'));
                    return false;
                }

                if (identifier == QStringLiteral("Header_End"))
                {
                    break;
                }
            }

            Decoder decoder;
            if (recordCount < 0 || recordType < 0
                || !std::isfinite(globalResolution) || globalResolution <= 0.0
                || !decoderForRecordType(static_cast<quint32>(recordType), decoder))
            {
                errorMessage = QStringLiteral("Unsupported or incomplete PTU measurement header");
                return false;
            }
            header.recordCount = static_cast<quint64>(recordCount);
            header.recordType = static_cast<quint32>(recordType);
            header.globalResolutionSeconds = globalResolution;
            header.decoder = decoder;
            return true;
        }

        SignalSourceDescriptor sourceDescriptor()
        {
            SignalSourceDescriptor descriptor;
            descriptor.id = QString::fromLatin1(kSourceId);
            descriptor.name = QStringLiteral("PTU File");
            descriptor.provider = QStringLiteral("PicoQuant");
            descriptor.quantity = QStringLiteral("Photon count");
            descriptor.unit = QStringLiteral("photons");
            descriptor.streamType = SignalStreamType::TimestampedEvents;

            SignalParameterDescriptor file;
            file.key = QStringLiteral("filePath");
            file.name = QStringLiteral("PTU file");
            file.type = SignalParameterType::File;
            file.fileFilter = QStringLiteral("PicoQuant PTU files (*.ptu);;All files (*)");
            descriptor.parameters.append(file);

            SignalParameterDescriptor channel;
            channel.key = QStringLiteral("detectorChannel");
            channel.name = QStringLiteral("Detector channel");
            channel.type = SignalParameterType::Integer;
            channel.defaultValue = 1;
            channel.hasRange = true;
            channel.minimum = 0;
            channel.maximum = 64;
            descriptor.parameters.append(channel);
            return descriptor;
        }

        bool readSettings(const SignalAcquisitionConfig& config,
                          PtuSettings& settings,
                          QString& errorMessage)
        {
            settings.filePath = config.sourceSettings
                                    .value(QStringLiteral("filePath"))
                                    .toString().trimmed();
            settings.detectorChannel = config.sourceSettings
                                           .value(QStringLiteral("detectorChannel"), 1)
                                           .toInt();
            settings.sampleIntervalSeconds = config.sampleIntervalSeconds;
            settings.publishEvents = config.publishTimestampedEvents
                || config.scanImage.enabled;
            if (!QFileInfo::exists(settings.filePath)
                || !QFileInfo(settings.filePath).isFile())
            {
                errorMessage = QStringLiteral("Select an existing PTU file");
            }
            else if (settings.detectorChannel < 0 || settings.detectorChannel > 64)
            {
                errorMessage = QStringLiteral("Detector channel must be between 0 and 64");
            }
            return errorMessage.isEmpty();
        }

        class PtuFileSource final : public SignalSource
        {
        public:
            explicit PtuFileSource(QObject* parent = nullptr)
                : SignalSource(parent)
            {
            }

            ~PtuFileSource() override
            {
                std::lock_guard lock(m_threadMutex);
                if (m_worker.joinable())
                {
                    m_worker.request_stop();
                    m_worker.join();
                }
            }

            bool start(const SignalAcquisitionConfig& config,
                       QString* errorMessage) override
            {
                PtuSettings settings;
                QString validationError;
                if (config.sourceId.trimmed() != QString::fromLatin1(kSourceId)
                    || !readSettings(config, settings, validationError))
                {
                    if (errorMessage)
                    {
                        *errorMessage = validationError.isEmpty()
                                            ? QStringLiteral("Invalid PTU source ID")
                                            : validationError;
                    }
                    return false;
                }

                std::lock_guard lock(m_threadMutex);
                const SignalSourceState currentState = m_state.load();
                if (currentState == SignalSourceState::Starting
                    || currentState == SignalSourceState::Running
                    || currentState == SignalSourceState::Stopping)
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("Signal source is already active");
                    }
                    return false;
                }
                if (m_worker.joinable())
                {
                    m_worker.join();
                }

                setState(SignalSourceState::Starting, QStringLiteral("Opening PTU file"));
                m_worker = std::jthread(
                    [this, settings](std::stop_token stopToken)
                    {
                        run(stopToken, settings);
                    });
                return true;
            }

            void stop() override
            {
                std::lock_guard lock(m_threadMutex);
                const SignalSourceState currentState = m_state.load();
                if (currentState != SignalSourceState::Starting
                    && currentState != SignalSourceState::Running)
                {
                    return;
                }
                setState(SignalSourceState::Stopping, QStringLiteral("Stopping PTU read"));
                m_worker.request_stop();
            }

            SignalSourceState state() const override
            {
                return m_state.load();
            }

            QString stateMessage() const override
            {
                std::lock_guard lock(m_stateMutex);
                return m_stateMessage;
            }

        private:
            void setState(SignalSourceState state, const QString& message)
            {
                {
                    std::lock_guard lock(m_stateMutex);
                    m_stateMessage = message;
                    m_state.store(state);
                }
                emit stateChanged(state, message);
            }

            void fail(const QString& message)
            {
                setState(SignalSourceState::Error, message);
                emit sourceError(message);
            }

            void run(std::stop_token stopToken, const PtuSettings& settings)
            {
                QFile file(settings.filePath);
                if (!file.open(QIODevice::ReadOnly))
                {
                    fail(QStringLiteral("Failed to open PTU file: %1").arg(file.errorString()));
                    return;
                }

                PtuHeader header;
                QString errorMessage;
                if (!readHeader(file, header, errorMessage))
                {
                    fail(errorMessage);
                    return;
                }
                if (header.recordCount
                    > static_cast<quint64>((std::numeric_limits<qint64>::max)() / 4)
                    || static_cast<quint64>(file.size() - file.pos())
                        < header.recordCount * 4)
                {
                    fail(QStringLiteral("PTU record data is incomplete"));
                    return;
                }

                setState(SignalSourceState::Running,
                         QStringLiteral("Reading %1 PTU records").arg(header.recordCount));
                EventCountBinner binner(QString::fromLatin1(kSourceId),
                                        QStringLiteral("Photon count"),
                                        QStringLiteral("photons"),
                                        header.globalResolutionSeconds,
                                        settings.sampleIntervalSeconds);
                const auto publishReadyChunks = [this, &binner]()
                {
                    for (const TimeSeriesChunk& chunk : binner.takeReadyChunks())
                    {
                        emit timeSeriesReady(chunk);
                    }
                };
                quint64 overflowCorrection = 0;
                quint64 processedRecords = 0;
                quint64 photonCount = 0;
                quint64 markerCount = 0;
                quint64 lastTick = 0;
                bool hasTick = false;

                while (processedRecords < header.recordCount
                       && !stopToken.stop_requested())
                {
                    const qsizetype recordsInChunk = static_cast<qsizetype>(std::min<quint64>(
                        kRecordsPerChunk, header.recordCount - processedRecords));
                    const QByteArray rawRecords = file.read(recordsInChunk * 4);
                    if (rawRecords.size() != recordsInChunk * 4)
                    {
                        fail(QStringLiteral("Unexpected end of PTU record data"));
                        return;
                    }

                    TimestampedEventChunk events;
                    if (settings.publishEvents)
                    {
                        events.sourceId = QString::fromLatin1(kSourceId);
                        events.tickPeriodSeconds = header.globalResolutionSeconds;
                        events.eventTicks.reserve(recordsInChunk);
                        events.eventCodes.reserve(recordsInChunk);
                    }
                    const auto addPhoton = [&](quint64 tick, quint32 channel)
                    {
                        lastTick = tick;
                        hasTick = true;
                        if (static_cast<int>(channel) == settings.detectorChannel)
                        {
                            binner.addEvent(tick);
                            ++photonCount;
                            if (settings.publishEvents)
                            {
                                events.eventTicks.append(tick);
                                events.eventCodes.append(channel);
                            }
                        }
                        else
                        {
                            binner.advanceToTick(tick);
                        }
                    };
                    const auto addMarker = [&](quint64 tick, quint32 code)
                    {
                        lastTick = tick;
                        hasTick = true;
                        binner.addMarker(tick, code);
                        ++markerCount;
                        if (settings.publishEvents)
                        {
                            events.markerTicks.append(tick);
                            events.markerCodes.append(code);
                        }
                    };
                    const auto advanceOverflow = [&](quint64 tick)
                    {
                        lastTick = tick;
                        hasTick = true;
                        binner.advanceToTick(tick);
                    };

                    const auto* bytes = reinterpret_cast<const uchar*>(rawRecords.constData());
                    for (qsizetype index = 0; index < recordsInChunk; ++index)
                    {
                        const quint32 record = qFromLittleEndian<quint32>(bytes + index * 4);
                        switch (header.decoder)
                        {
                        case Decoder::PicoT2:
                        {
                            constexpr quint64 wraparound = 210698240ULL;
                            const quint32 channel = record >> 28;
                            const quint32 time = record & 0x0fffffffU;
                            if (channel != 0x0fU)
                            {
                                addPhoton(overflowCorrection + time, channel);
                            }
                            else if ((time & 0x0fU) == 0)
                            {
                                overflowCorrection += wraparound;
                                advanceOverflow(overflowCorrection);
                            }
                            else
                            {
                                addMarker(overflowCorrection + (time & 0x0ffffff0U),
                                          time & 0x0fU);
                            }
                            break;
                        }
                        case Decoder::PicoT3:
                        {
                            constexpr quint64 wraparound = 65536ULL;
                            const quint32 channel = record >> 28;
                            const quint32 nsync = record & 0x0000ffffU;
                            const quint32 dtime = (record >> 16) & 0x00000fffU;
                            if (channel != 0x0fU)
                            {
                                addPhoton(overflowCorrection + nsync, channel);
                            }
                            else if (dtime == 0)
                            {
                                overflowCorrection += wraparound;
                                advanceOverflow(overflowCorrection);
                            }
                            else
                            {
                                addMarker(overflowCorrection + nsync, dtime);
                            }
                            break;
                        }
                        case Decoder::HydraT2V1:
                        case Decoder::HydraT2V2:
                        {
                            constexpr quint64 wraparoundV1 = 33552000ULL;
                            constexpr quint64 wraparoundV2 = 33554432ULL;
                            const bool version1 = header.decoder == Decoder::HydraT2V1;
                            const bool special = (record & 0x80000000U) != 0;
                            const quint32 channel = (record >> 25) & 0x3fU;
                            const quint32 time = record & 0x01ffffffU;
                            if (!special)
                            {
                                addPhoton(overflowCorrection + time, channel + 1);
                            }
                            else if (channel == 0x3fU)
                            {
                                const quint64 overflows = version1 || time == 0 ? 1 : time;
                                overflowCorrection += overflows
                                    * (version1 ? wraparoundV1 : wraparoundV2);
                                advanceOverflow(overflowCorrection);
                            }
                            else if (channel >= 1 && channel <= 15)
                            {
                                addMarker(overflowCorrection + time, channel);
                            }
                            else if (channel == 0)
                            {
                                addPhoton(overflowCorrection + time, 0);
                            }
                            break;
                        }
                        case Decoder::HydraT3V1:
                        case Decoder::HydraT3V2:
                        {
                            constexpr quint64 wraparound = 1024ULL;
                            const bool version1 = header.decoder == Decoder::HydraT3V1;
                            const bool special = (record & 0x80000000U) != 0;
                            const quint32 channel = (record >> 25) & 0x3fU;
                            const quint32 nsync = record & 0x000003ffU;
                            if (!special)
                            {
                                addPhoton(overflowCorrection + nsync, channel);
                            }
                            else if (channel == 0x3fU)
                            {
                                const quint64 overflows = version1 || nsync == 0 ? 1 : nsync;
                                overflowCorrection += overflows * wraparound;
                                advanceOverflow(overflowCorrection);
                            }
                            else if (channel >= 1 && channel <= 15)
                            {
                                addMarker(overflowCorrection + nsync, channel);
                            }
                            break;
                        }
                        }
                        if (binner.hasReadyChunks())
                        {
                            publishReadyChunks();
                        }
                    }
                    processedRecords += static_cast<quint64>(recordsInChunk);
                    if (events.isValid())
                    {
                        emit timestampedEventsReady(events);
                    }
                    for (const TimeSeriesChunk& chunk : binner.takeCompletedChunks())
                    {
                        emit timeSeriesReady(chunk);
                    }
                }

                if (stopToken.stop_requested())
                {
                    setState(SignalSourceState::Idle, QStringLiteral("PTU read stopped"));
                    return;
                }
                if (hasTick)
                {
                    binner.advanceToElapsedSeconds(
                        static_cast<double>(lastTick) * header.globalResolutionSeconds
                        + settings.sampleIntervalSeconds);
                    for (const TimeSeriesChunk& chunk : binner.takeCompletedChunks())
                    {
                        emit timeSeriesReady(chunk);
                    }
                }
                setState(SignalSourceState::Idle,
                         QStringLiteral("PTU read complete: %1 photons, %2 markers")
                             .arg(photonCount)
                             .arg(markerCount));
            }

            mutable std::mutex m_stateMutex;
            std::mutex m_threadMutex;
            std::jthread m_worker;
            std::atomic<SignalSourceState> m_state{SignalSourceState::Idle};
            QString m_stateMessage{QStringLiteral("PTU file source is idle")};
        };
    }

    QList<SignalSourceDescriptor> PtuFilePlugin::signalSources() const
    {
        return {sourceDescriptor()};
    }

    SignalSource* PtuFilePlugin::createSignalSource(const QString& sourceId,
                                                    QObject* parent)
    {
        return sourceId.trimmed() == QString::fromLatin1(kSourceId)
                   ? new PtuFileSource(parent)
                   : nullptr;
    }
}
