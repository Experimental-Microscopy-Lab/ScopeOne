#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QtEndian>

namespace scopeone::core::internal::driverhost
{
    inline constexpr quint32 kProtocolVersion = 5;
    inline constexpr quint32 kMaxControlMessageBytes = 256 * 1024;

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

    inline const QString kMessageKindRequest = QStringLiteral("Request");
    inline const QString kMessageKindResponse = QStringLiteral("Response");
    inline const QString kMessageKindEvent = QStringLiteral("Event");

    inline const QString kCommandDescribe = QStringLiteral("Describe");
    inline const QString kCommandShutdown = QStringLiteral("Shutdown");
    inline const QString kCommandStartPreview = QStringLiteral("StartPreview");
    inline const QString kCommandStopPreview = QStringLiteral("StopPreview");
    inline const QString kCommandSetFrameDeliveryMode = QStringLiteral("SetFrameDeliveryMode");
    inline const QString kCommandGetExposure = QStringLiteral("GetExposure");
    inline const QString kCommandSetExposure = QStringLiteral("SetExposure");
    inline const QString kCommandListProperties = QStringLiteral("ListProperties");
    inline const QString kCommandGetProperty = QStringLiteral("GetProperty");
    inline const QString kCommandSetProperty = QStringLiteral("SetProperty");
    inline const QString kCommandCaptureEvent = QStringLiteral("CaptureEvent");
    inline const QString kCommandSetRoi = QStringLiteral("SetROI");
    inline const QString kCommandClearRoi = QStringLiteral("ClearROI");
    inline const QString kCommandGetRoi = QStringLiteral("GetROI");
    inline const QString kCommandGetXYPosition = QStringLiteral("GetXYPosition");
    inline const QString kCommandGetZPosition = QStringLiteral("GetZPosition");
    inline const QString kCommandSetRelativeXYPosition = QStringLiteral("SetRelativeXYPosition");
    inline const QString kCommandSetRelativeZPosition = QStringLiteral("SetRelativeZPosition");
    inline const QString kCommandSetXYPosition = QStringLiteral("SetXYPosition");
    inline const QString kCommandSetZPosition = QStringLiteral("SetZPosition");
    inline const QString kCommandGetShutterOpen = QStringLiteral("GetShutterOpen");
    inline const QString kCommandSetShutterOpen = QStringLiteral("SetShutterOpen");
    inline const QString kCommandGetState = QStringLiteral("GetState");
    inline const QString kCommandSetState = QStringLiteral("SetState");
    inline const QString kCommandGetStateLabel = QStringLiteral("GetStateLabel");
    inline const QString kCommandListConfigGroups = QStringLiteral("ListConfigGroups");
    inline const QString kCommandListConfigs = QStringLiteral("ListConfigs");
    inline const QString kCommandGetCurrentConfig = QStringLiteral("GetCurrentConfig");
    inline const QString kCommandSetConfig = QStringLiteral("SetConfig");

    inline const QString kCapabilityCamera = QStringLiteral("Camera");
    inline const QString kCapabilityProperties = QStringLiteral("Properties");
    inline const QString kCapabilityStage = QStringLiteral("Stage");
    inline const QString kCapabilityShutter = QStringLiteral("Shutter");
    inline const QString kCapabilityState = QStringLiteral("State");
    inline const QString kCapabilityConfiguration = QStringLiteral("Configuration");

    inline const QString kFrameDeliveryModePreviewLatest = QStringLiteral("PreviewLatest");
    inline const QString kFrameDeliveryModeLatestOnly = QStringLiteral("LatestOnly");
    inline const QString kFrameDeliveryModeAllFrames = QStringLiteral("AllFrames");

    inline const QString kEventHello = QStringLiteral("Hello");
    inline const QString kEventFrameAvailable = QStringLiteral("FrameAvailable");
    inline const QString kEventPreviewState = QStringLiteral("PreviewState");
    inline const QString kEventDriverHostError = QStringLiteral("DriverHostError");

#ifdef Q_OS_WIN
    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_DriverHost.exe");
#else
    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_DriverHost");
#endif

    inline QString controlServerName(const QString& deviceId)
    {
        return QStringLiteral("ScopeOne.DriverHost.%1.ctrl").arg(deviceId);
    }

    inline QString sharedMemoryKey(const QString& deviceId)
    {
        return QStringLiteral("ScopeOne.DriverHost.%1.shm").arg(deviceId);
    }

    inline QString sharedMemoryKey(const QString& hostKey, int cameraIndex)
    {
        return QStringLiteral("ScopeOne.DriverHost.%1.%2.shm")
            .arg(hostKey)
            .arg(cameraIndex);
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
