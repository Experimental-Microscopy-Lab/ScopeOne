#pragma once

#include "scopeone/HardwareTypes.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QSharedMemory>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QtEndian>

#include <utility>

namespace scopeone::core::internal::driverhost
{
    inline constexpr quint32 kProtocolVersion = 8;

    // The names of the device kinds as the protocol sends them
    inline constexpr std::pair<HardwareDeviceKind, const char*> kDeviceKindNames[]{
        {HardwareDeviceKind::Camera, "Camera"},
        {HardwareDeviceKind::XYStage, "XYStage"},
        {HardwareDeviceKind::ZStage, "ZStage"},
        {HardwareDeviceKind::Shutter, "Shutter"},
        {HardwareDeviceKind::State, "State"},
        {HardwareDeviceKind::Hub, "Hub"},
        {HardwareDeviceKind::Serial, "Serial"},
        {HardwareDeviceKind::Generic, "Generic"},
        {HardwareDeviceKind::AutoFocus, "AutoFocus"},
        {HardwareDeviceKind::ImageProcessor, "ImageProcessor"},
        {HardwareDeviceKind::SignalIO, "SignalIO"},
        {HardwareDeviceKind::Magnifier, "Magnifier"},
        {HardwareDeviceKind::SLM, "SLM"},
        {HardwareDeviceKind::Galvo, "Galvo"},
        {HardwareDeviceKind::PressurePump, "PressurePump"},
        {HardwareDeviceKind::VolumetricPump, "VolumetricPump"}};

    inline QString deviceKindName(HardwareDeviceKind kind)
    {
        for (const auto& [value, name] : kDeviceKindNames)
        {
            if (value == kind)
            {
                return QLatin1String(name);
            }
        }
        return QStringLiteral("Unknown");
    }

    inline HardwareDeviceKind deviceKindFromName(const QString& text)
    {
        for (const auto& [value, name] : kDeviceKindNames)
        {
            if (text == QLatin1String(name))
            {
                return value;
            }
        }
        return HardwareDeviceKind::Unknown;
    }
    // A DAQ session carries its output waveforms in the control message
    inline constexpr quint32 kMaxControlMessageBytes = 64 * 1024 * 1024;

    inline const QString kEnvelopeKindField = QStringLiteral("kind");
    inline const QString kEnvelopeVersionField = QStringLiteral("version");
    inline const QString kEnvelopeRequestIdField = QStringLiteral("requestId");
    inline const QString kMessageTypeField = QStringLiteral("type");
    inline const QString kProviderIdField = QStringLiteral("providerId");
    inline const QString kDeviceIdField = QStringLiteral("deviceId");
    inline const QString kDeviceKindField = QStringLiteral("deviceKind");
    inline const QString kCapabilitiesField = QStringLiteral("capabilities");
    inline const QString kDevicesField = QStringLiteral("devices");
    inline const QString kProviderNameField = QStringLiteral("providerName");
    inline const QString kProviderVersionField = QStringLiteral("providerVersion");
    inline const QString kProviderDeviceIdField = QStringLiteral("providerDeviceId");
    inline const QString kHardwareIdField = QStringLiteral("hardwareId");
    inline const QString kDeviceNameField = QStringLiteral("deviceName");
    inline const QString kDeviceStateField = QStringLiteral("deviceState");
    inline const QString kDevicePropertiesField = QStringLiteral("deviceProperties");
    inline const QString kSharedMemoryKeyField = QStringLiteral("sharedMemoryKey");
    inline const QString kDefaultXYStageField = QStringLiteral("defaultXYStage");
    inline const QString kDefaultZStageField = QStringLiteral("defaultZStage");
    inline const QString kDefaultShutterField = QStringLiteral("defaultShutter");
    inline const QString kDefaultAutofocusField = QStringLiteral("defaultAutofocus");
    // Whether a camera takes a series of images in one sequence acquisition
    inline const QString kEventSequenceField = QStringLiteral("eventSequence");
    inline const QString kPixelSizeField = QStringLiteral("pixelSizeUm");
    inline const QString kSignalSourcesField = QStringLiteral("signalSources");
    inline const QString kDaqDevicesField = QStringLiteral("daqDevices");
    inline const QString kSourceIdField = QStringLiteral("sourceId");
    inline const QString kStreamIdField = QStringLiteral("streamId");
    inline const QString kConfigField = QStringLiteral("config");
    inline const QString kStateField = QStringLiteral("state");
    inline const QString kStateMessageField = QStringLiteral("message");

    inline const QString kMessageKindRequest = QStringLiteral("Request");
    inline const QString kMessageKindResponse = QStringLiteral("Response");
    inline const QString kMessageKindEvent = QStringLiteral("Event");

    inline const QString kCommandDescribe = QStringLiteral("Describe");
    inline const QString kCommandShutdown = QStringLiteral("Shutdown");
    inline const QString kCommandStartPreview = QStringLiteral("StartPreview");
    inline const QString kCommandStopPreview = QStringLiteral("StopPreview");
    inline const QString kCommandSetFrameDeliveryMode = QStringLiteral("SetFrameDeliveryMode");
    inline const QString kCommandSetFrameDeliveryPaused = QStringLiteral("SetFrameDeliveryPaused");
    // ScopeOne has attached the rings named in "keys", so DriverHost removes their names. The
    // memory stays mapped on both ends and goes away with the last of them, even after a crash.
    inline const QString kCommandReleaseSharedMemory = QStringLiteral("ReleaseSharedMemory");
    inline const QString kKeysField = QStringLiteral("keys");
    inline const QString kCommandGetExposure = QStringLiteral("GetExposure");
    inline const QString kCommandSetExposure = QStringLiteral("SetExposure");
    inline const QString kCommandListProperties = QStringLiteral("ListProperties");
    inline const QString kCommandGetProperty = QStringLiteral("GetProperty");
    inline const QString kCommandSetProperty = QStringLiteral("SetProperty");
    inline const QString kCommandCaptureEvent = QStringLiteral("CaptureEvent");
    // The images of a sequence wait in DriverHost, and SendEventFrames writes them to the frame
    // ring a part at a time, as the ring holds fewer images than a sequence can have
    inline const QString kCommandCaptureEventSequence = QStringLiteral("CaptureEventSequence");
    inline const QString kCommandSendEventFrames = QStringLiteral("SendEventFrames");
    inline const QString kCommandGetPixelSize = QStringLiteral("GetPixelSize");
    inline const QString kCommandSetRoi = QStringLiteral("SetROI");
    inline const QString kCommandClearRoi = QStringLiteral("ClearROI");
    inline const QString kCommandGetRoi = QStringLiteral("GetROI");
    inline const QString kCommandGetXYPosition = QStringLiteral("GetXYPosition");
    inline const QString kCommandGetZPosition = QStringLiteral("GetZPosition");
    inline const QString kCommandSetRelativeXYPosition = QStringLiteral("SetRelativeXYPosition");
    inline const QString kCommandSetRelativeZPosition = QStringLiteral("SetRelativeZPosition");
    inline const QString kCommandSetXYPosition = QStringLiteral("SetXYPosition");
    inline const QString kCommandSetZPosition = QStringLiteral("SetZPosition");
    inline const QString kCommandGetStageSequenceCapacity = QStringLiteral("GetStageSequenceCapacity");
    inline const QString kCommandStartStageSequence = QStringLiteral("StartStageSequence");
    inline const QString kCommandStopStageSequence = QStringLiteral("StopStageSequence");
    inline const QString kCommandGetPropertySequenceCapacity = QStringLiteral("GetPropertySequenceCapacity");
    inline const QString kCommandStartPropertySequence = QStringLiteral("StartPropertySequence");
    inline const QString kCommandStopPropertySequence = QStringLiteral("StopPropertySequence");
    inline const QString kCommandGetAutoShutter = QStringLiteral("GetAutoShutter");
    inline const QString kCommandSetAutoShutter = QStringLiteral("SetAutoShutter");
    inline const QString kCommandRunAutofocus = QStringLiteral("RunAutofocus");
    inline const QString kCommandGetShutterOpen = QStringLiteral("GetShutterOpen");
    inline const QString kCommandSetShutterOpen = QStringLiteral("SetShutterOpen");
    inline const QString kCommandGetState = QStringLiteral("GetState");
    inline const QString kCommandSetState = QStringLiteral("SetState");
    inline const QString kCommandGetStateLabel = QStringLiteral("GetStateLabel");
    inline const QString kCommandListConfigGroups = QStringLiteral("ListConfigGroups");
    inline const QString kCommandListConfigs = QStringLiteral("ListConfigs");
    inline const QString kCommandGetCurrentConfig = QStringLiteral("GetCurrentConfig");
    inline const QString kCommandSetConfig = QStringLiteral("SetConfig");
    inline const QString kCommandGetConfigSettings = QStringLiteral("GetConfigSettings");
    inline const QString kCommandStartSignal = QStringLiteral("StartSignal");
    inline const QString kCommandStopSignal = QStringLiteral("StopSignal");
    inline const QString kCommandStartDaq = QStringLiteral("StartDaq");
    inline const QString kCommandStopDaq = QStringLiteral("StopDaq");

    inline const QString kCapabilityCamera = QStringLiteral("Camera");
    inline const QString kCapabilityProperties = QStringLiteral("Properties");
    inline const QString kCapabilityStage = QStringLiteral("Stage");
    inline const QString kCapabilityShutter = QStringLiteral("Shutter");
    inline const QString kCapabilityState = QStringLiteral("State");
    inline const QString kCapabilityConfiguration = QStringLiteral("Configuration");
    inline const QString kCapabilityAutofocus = QStringLiteral("Autofocus");

    inline const QString kFrameDeliveryModePreviewLatest = QStringLiteral("PreviewLatest");
    inline const QString kFrameDeliveryModeLatestOnly = QStringLiteral("LatestOnly");
    inline const QString kFrameDeliveryModeAllFrames = QStringLiteral("AllFrames");

    inline const QString kEventHello = QStringLiteral("Hello");
    inline const QString kEventFrameAvailable = QStringLiteral("FrameAvailable");
    inline const QString kEventPreviewState = QStringLiteral("PreviewState");
    inline const QString kEventDriverHostError = QStringLiteral("DriverHostError");
    // New records in the stream ring of a signal source or a DAQ device
    inline const QString kEventStreamAvailable = QStringLiteral("StreamAvailable");
    inline const QString kEventSignalState = QStringLiteral("SignalState");
    inline const QString kEventDaqState = QStringLiteral("DaqState");

#ifdef Q_OS_WIN
    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_DriverHost.exe");
#else
    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_DriverHost");
#endif

    // The names of a host on macOS have to be short: a unix socket path holds at most 104
    // characters, with the temporary folder taking about half, and Qt cuts a POSIX shared memory
    // name to 30, which gave two cameras of one host the same name. The host key is hashed to 16
    // characters for them.
    inline QString hostHash(const QString& hostKey)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(hostKey.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    }

    inline QString controlServerName(const QString& hostKey)
    {
        return QStringLiteral("ScopeOne.%1.ctrl").arg(hostHash(hostKey));
    }

    inline QString sharedMemoryKey(const QString& hostKey, int cameraIndex)
    {
        return QStringLiteral("so.%1.c%2").arg(hostHash(hostKey)).arg(cameraIndex);
    }

    // Each start of a stream gets a ring of its own, sized for what that acquisition delivers
    inline QString streamSharedMemoryKey(const QString& hostKey, quint64 streamIndex)
    {
        return QStringLiteral("so.%1.s%2").arg(hostHash(hostKey)).arg(streamIndex);
    }

    // POSIX shared memory on Linux and macOS. The System V memory that Qt uses by default holds
    // at most 4 MB in one block on macOS, too little for a frame ring. Qt makes the name fit the
    // limits of the platform, 31 characters on macOS.
    inline QNativeIpcKey sharedMemoryNativeKey(const QString& key)
    {
#ifdef Q_OS_WIN
        return QSharedMemory::platformSafeKey(key, QNativeIpcKey::Type::Windows);
#else
        return QSharedMemory::platformSafeKey(key, QNativeIpcKey::Type::PosixRealtime);
#endif
    }

    inline QString encodeUInt64(quint64 value)
    {
        return QString::number(value);
    }

    inline quint64 decodeUInt64(const QJsonValue& value, quint64 defaultValue = 0)
    {
        if (value.isString())
        {
            bool ok = false;
            const quint64 parsed = value.toString().toULongLong(&ok);
            return ok ? parsed : defaultValue;
        }
        if (value.isDouble())
        {
            const double numeric = value.toDouble(static_cast<double>(defaultValue));
            return numeric >= 0.0 ? static_cast<quint64>(numeric) : defaultValue;
        }
        return defaultValue;
    }

    inline QJsonObject makeEnvelope(const QString& kind,
                                    const QString& type,
                                    quint64 requestId = 0)
    {
        QJsonObject object;
        object.insert(kEnvelopeKindField, kind);
        object.insert(kEnvelopeVersionField, static_cast<int>(kProtocolVersion));
        object.insert(kMessageTypeField, type);
        if (requestId != 0)
        {
            object.insert(kEnvelopeRequestIdField, encodeUInt64(requestId));
        }
        return object;
    }

    inline QByteArray encodeMessage(const QJsonObject& message)
    {
        const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
        QByteArray framed;
        framed.resize(static_cast<int>(sizeof(quint32)));
        qToLittleEndian<quint32>(static_cast<quint32>(payload.size()),
                                 reinterpret_cast<uchar*>(framed.data()));
        framed += payload;
        return framed;
    }

    enum class DecodeResult
    {
        Incomplete,
        Complete,
        Error
    };

    inline DecodeResult tryDecodeMessage(QByteArray& buffer,
                                         QJsonObject& message,
                                         QString* error = nullptr)
    {
        if (buffer.size() < static_cast<int>(sizeof(quint32)))
        {
            return DecodeResult::Incomplete;
        }
        const quint32 payloadSize =
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData()));
        if (payloadSize == 0 || payloadSize > kMaxControlMessageBytes)
        {
            if (error)
            {
                *error = QStringLiteral("Invalid control message size");
            }
            buffer.clear();
            return DecodeResult::Error;
        }
        const int frameSize = static_cast<int>(sizeof(quint32) + payloadSize);
        if (buffer.size() < frameSize)
        {
            return DecodeResult::Incomplete;
        }

        const QByteArray payload = buffer.mid(static_cast<int>(sizeof(quint32)),
                                              static_cast<int>(payloadSize));
        buffer.remove(0, frameSize);
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("Malformed control message payload");
            }
            return DecodeResult::Error;
        }
        message = document.object();
        return DecodeResult::Complete;
    }
}
