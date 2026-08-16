#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtEndian>

namespace scopeone::core::internal::driverhost
{
    inline constexpr quint32 kProtocolVersion = 3;
    inline constexpr quint32 kMaxControlMessageBytes = 256 * 1024;

    inline const QString kEnvelopeKindField = QStringLiteral("kind");
    inline const QString kEnvelopeVersionField = QStringLiteral("version");
    inline const QString kEnvelopeRequestIdField = QStringLiteral("requestId");
    inline const QString kMessageTypeField = QStringLiteral("type");

    inline const QString kMessageKindRequest = QStringLiteral("Request");
    inline const QString kMessageKindResponse = QStringLiteral("Response");
    inline const QString kMessageKindEvent = QStringLiteral("Event");

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
