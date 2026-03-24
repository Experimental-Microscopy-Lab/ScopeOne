#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtEndian>

namespace scopeone::core::internal::agent
{
    inline constexpr quint32 kProtocolVersion = 1;
    inline constexpr quint32 kMaxControlMessageBytes = 256 * 1024;

    inline const QString kEnvelopeKindField = QStringLiteral("kind");
    inline const QString kEnvelopeVersionField = QStringLiteral("version");
    inline const QString kEnvelopeRequestIdField = QStringLiteral("requestId");
    inline const QString kMessageTypeField = QStringLiteral("type");

    inline const QString kMessageKindRequest = QStringLiteral("Request");
    inline const QString kMessageKindResponse = QStringLiteral("Response");
    inline const QString kMessageKindEvent = QStringLiteral("Event");

    inline const QString kCommandShutdown = QStringLiteral("Shutdown");
    inline const QString kCommandStartPreview = QStringLiteral("StartPreview");
    inline const QString kCommandStopPreview = QStringLiteral("StopPreview");
    inline const QString kCommandSetExposure = QStringLiteral("SetExposure");
    inline const QString kCommandListProperties = QStringLiteral("ListProperties");
    inline const QString kCommandGetProperty = QStringLiteral("GetProperty");
    inline const QString kCommandSetProperty = QStringLiteral("SetProperty");
    inline const QString kCommandCaptureEvent = QStringLiteral("CaptureEvent");
    inline const QString kCommandSetRoi = QStringLiteral("SetROI");
    inline const QString kCommandClearRoi = QStringLiteral("ClearROI");
    inline const QString kCommandGetRoi = QStringLiteral("GetROI");

    inline const QString kEventHello = QStringLiteral("Hello");
    inline const QString kEventFrameAvailable = QStringLiteral("FrameAvailable");
    inline const QString kEventPreviewState = QStringLiteral("PreviewState");
    inline const QString kEventAgentError = QStringLiteral("AgentError");
    inline const QString kEventBufferOverflow = QStringLiteral("BufferOverflow");

    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_Agent.exe");

    inline QString controlServerName(const QString& cameraId)
    {
        return QStringLiteral("ScopeOne.%1.ctrl").arg(cameraId);
    }

    inline QString sharedMemoryKey(const QString& cameraId)
    {
        return QStringLiteral("ScopeOne.%1.shm").arg(cameraId);
    }

    inline QString encodeUInt64(quint64 value)
    {
        return QString::number(value);
    }

    inline quint64 decodeUInt64(const QJsonValue& value, quint64 fallback = 0)
    {
        if (value.isString())
        {
            bool ok = false;
            const quint64 parsed = value.toString().toULongLong(&ok);
            return ok ? parsed : fallback;
        }
        if (value.isDouble())
        {
            const double numeric = value.toDouble(static_cast<double>(fallback));
            return (numeric >= 0.0) ? static_cast<quint64>(numeric) : fallback;
        }
        return fallback;
    }

    inline QJsonObject makeEnvelope(const QString& kind,
                                    const QString& type,
                                    quint64 requestId = 0)
    {
        QJsonObject obj;
        obj.insert(kEnvelopeKindField, kind);
        obj.insert(kEnvelopeVersionField, static_cast<int>(kProtocolVersion));
        obj.insert(kMessageTypeField, type);
        if (requestId != 0)
        {
            obj.insert(kEnvelopeRequestIdField, encodeUInt64(requestId));
        }
        return obj;
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

    inline DecodeResult tryDecodeMessage(QByteArray& buffer, QJsonObject& message, QString* error = nullptr)
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

        const QByteArray payload = buffer.mid(static_cast<int>(sizeof(quint32)), static_cast<int>(payloadSize));
        buffer.remove(0, frameSize);

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        {
            if (error)
            {
                *error = QStringLiteral("Malformed control message payload");
            }
            return DecodeResult::Error;
        }

        message = doc.object();
        return DecodeResult::Complete;
    }
} // namespace scopeone::core::internal::agent
