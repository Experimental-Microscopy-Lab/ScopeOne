#include "SerialDevice.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSerialPortInfo>

#include <cmath>
#include <utility>

namespace serial_driver
{
    namespace
    {
        const QString kPort = QStringLiteral("Port");
        const QString kBaudRate = QStringLiteral("BaudRate");
        const QString kAnswerTimeout = QStringLiteral("AnswerTimeout");
        const QString kDescriptionFile = QStringLiteral("Description File");

        bool isPreInitName(const QString& name)
        {
            return name == kPort || name == kBaudRate || name == kAnswerTimeout || name == kDescriptionFile;
        }
    }

    std::optional<SerialProfile> loadSerialProfile(const QString& path,
                                                   const QString& deviceName,
                                                   QString* errorMessage)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            *errorMessage = QStringLiteral("Cannot read the description file %1").arg(path);
            return std::nullopt;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            *errorMessage = QStringLiteral("The description file %1 is not a JSON object: %2")
                                .arg(path, parseError.errorString());
            return std::nullopt;
        }

        const QJsonObject root = document.object();
        SerialProfile profile;
        profile.name = root.value(QStringLiteral("name")).toString();
        for (const QJsonValue& value : root.value(QStringLiteral("devices")).toArray())
        {
            profile.devices.append(value.toString());
        }
        if (!profile.devices.contains(deviceName))
        {
            *errorMessage = QStringLiteral("%1 has no device %2").arg(profile.name, deviceName);
            return std::nullopt;
        }

        const QJsonObject serial = root.value(QStringLiteral("serial")).toObject();
        profile.baudRate = serial.value(QStringLiteral("BaudRate")).toInt(profile.baudRate);
        const QString parity = serial.value(QStringLiteral("parity")).toString(QStringLiteral("none"));
        profile.parity = parity == QStringLiteral("even") ? QSerialPort::EvenParity
                       : parity == QStringLiteral("odd") ? QSerialPort::OddParity
                                                         : QSerialPort::NoParity;
        profile.stopBits = serial.value(QStringLiteral("stopBits")).toInt(1) == 2
                               ? QSerialPort::TwoStop
                               : QSerialPort::OneStop;
        const QString flowControl = serial.value(QStringLiteral("flowControl")).toString(QStringLiteral("none"));
        profile.flowControl = flowControl == QStringLiteral("hardware") ? QSerialPort::HardwareControl
                            : flowControl == QStringLiteral("software") ? QSerialPort::SoftwareControl
                                                                        : QSerialPort::NoFlowControl;

        const QJsonObject terminator = root.value(QStringLiteral("terminator")).toObject();
        profile.writeTerminator = terminator.value(QStringLiteral("write")).toString();
        profile.readTerminator = terminator.value(QStringLiteral("read")).toString();
        profile.setReply = root.value(QStringLiteral("setReply")).toString();
        profile.errorPattern = root.value(QStringLiteral("error")).toString();
        profile.ack = root.value(QStringLiteral("ack")).toString();
        profile.answerTimeout = root.value(QStringLiteral("AnswerTimeout")).toInt(profile.answerTimeout);

        const QJsonObject properties = root.value(QStringLiteral("properties")).toObject();
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        {
            const QJsonObject object = it.value().toObject();
            SerialProperty property;
            property.type = object.value(QStringLiteral("type")).toString();
            property.get = object.value(QStringLiteral("get")).toString();
            property.set = object.value(QStringLiteral("set")).toString();
            property.parse = object.value(QStringLiteral("parse")).toString();
            const QJsonObject values = object.value(QStringLiteral("values")).toObject();
            for (auto value = values.constBegin(); value != values.constEnd(); ++value)
            {
                property.values.append({value.key(), value.value().toString()});
            }
            const QJsonValue minimum = object.value(QStringLiteral("min"));
            if (minimum.isDouble())
            {
                property.min = minimum.toDouble();
            }
            // A maximum is either a number or a query that the device answers once at creation
            const QJsonValue maximum = object.value(QStringLiteral("max"));
            if (maximum.isDouble())
            {
                property.max = maximum.toDouble();
            }
            else
            {
                property.maxGet = maximum.toObject().value(QStringLiteral("get")).toString();
            }
            profile.properties.insert(it.key(), property);
        }

        const QJsonObject shutter = root.value(QStringLiteral("shutter")).toObject();
        if (!shutter.isEmpty())
        {
            profile.shutter = SerialShutter{shutter.value(QStringLiteral("property")).toString(),
                                            shutter.value(QStringLiteral("open")).toString(),
                                            shutter.value(QStringLiteral("closed")).toString()};
        }
        return profile;
    }

    DevicePtr createSerialDevice(const QString& name,
                                 const QVariantMap& preInitProperties,
                                 QString* errorMessage)
    {
        const QString descriptionPath = preInitProperties.value(kDescriptionFile).toString();
        std::optional<SerialProfile> loaded = loadSerialProfile(descriptionPath, name, errorMessage);
        if (!loaded)
        {
            return {};
        }
        SerialProfile profile = std::move(*loaded);
        profile.baudRate = preInitProperties.value(kBaudRate, profile.baudRate).toInt();
        profile.answerTimeout = preInitProperties.value(kAnswerTimeout, profile.answerTimeout).toInt();
        const QString port = preInitProperties.value(kPort).toString();
        if (port.isEmpty())
        {
            *errorMessage = QStringLiteral("The serial device has no Port");
            return {};
        }

        std::shared_ptr<SerialDevice> device;
        if (profile.shutter)
        {
            device = std::make_shared<SerialShutterDevice>(std::move(profile), port, descriptionPath);
        }
        else
        {
            device = std::make_shared<SerialDevice>(std::move(profile), port, descriptionPath);
        }
        if (!device->open(errorMessage))
        {
            return {};
        }
        return device;
    }

    SerialDevice::SerialDevice(SerialProfile profile, const QString& portName, const QString& descriptionPath)
        : m_profile(std::move(profile))
    {
        // The pre-init properties show what the device was created with
        m_cache.insert(kPort, portName);
        m_cache.insert(kBaudRate, QString::number(m_profile.baudRate));
        m_cache.insert(kAnswerTimeout, QString::number(m_profile.answerTimeout));
        m_cache.insert(kDescriptionFile, descriptionPath);
    }

    bool SerialDevice::open(QString* errorMessage)
    {
        const QString portName = m_cache.value(kPort);
        m_port.setPortName(portName);
        m_port.setBaudRate(m_profile.baudRate);
        m_port.setDataBits(QSerialPort::Data8);
        m_port.setParity(m_profile.parity);
        m_port.setStopBits(m_profile.stopBits);
        m_port.setFlowControl(m_profile.flowControl);
        if (!m_port.open(QIODevice::ReadWrite))
        {
            QStringList available;
            for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
            {
                available.append(info.portName());
            }
            *errorMessage = QStringLiteral("Cannot open the serial port %1: %2. Serial ports: %3")
                                .arg(portName, m_port.errorString(),
                                     available.isEmpty() ? QStringLiteral("none") : available.join(QStringLiteral(", ")));
            return false;
        }

        for (auto it = m_profile.properties.begin(); it != m_profile.properties.end(); ++it)
        {
            if (it->maxGet.isEmpty())
            {
                continue;
            }
            QString reply;
            if (!exchange(it->maxGet, &reply, errorMessage))
            {
                return false;
            }
            bool ok = false;
            const double maximum = reply.toDouble(&ok);
            if (!ok)
            {
                *errorMessage = QStringLiteral("Reply \"%1\" to \"%2\" from %3 is not a number")
                                    .arg(reply, it->maxGet, portName);
                return false;
            }
            it->max = maximum;
        }
        return true;
    }

    HardwareProviderDescriptor SerialDevice::descriptor() const
    {
        return {QString{}, m_profile.name, QStringLiteral("1")};
    }

    QStringList SerialDevice::listProperties(const QString&)
    {
        QStringList names{kPort, kBaudRate, kAnswerTimeout, kDescriptionFile};
        names.append(m_profile.properties.keys());
        return names;
    }

    QString SerialDevice::getProperty(const QString&, const QString& name, bool fromCache)
    {
        const auto it = m_profile.properties.constFind(name);
        if (fromCache || it == m_profile.properties.cend() || it->get.isEmpty())
        {
            return m_cache.value(name);
        }
        QString value;
        QString errorMessage;
        if (!readValue(name, &value, &errorMessage))
        {
            // The last value the device answered stays as the value of the property
            qWarning().noquote() << errorMessage;
            return m_cache.value(name);
        }
        return value;
    }

    bool SerialDevice::setProperty(const QString&,
                                   const QString& name,
                                   const QString& value,
                                   QString* errorMessage)
    {
        QString unused;
        return setValue(name, value, errorMessage ? errorMessage : &unused);
    }

    QString SerialDevice::getPropertyType(const QString&, const QString& name)
    {
        if (isPreInitName(name))
        {
            return name == kBaudRate || name == kAnswerTimeout ? QStringLiteral("Integer") : QStringLiteral("String");
        }
        const auto it = m_profile.properties.constFind(name);
        if (it == m_profile.properties.cend())
        {
            return {};
        }
        if (it->type == QStringLiteral("float"))
        {
            return QStringLiteral("Float");
        }
        if (it->type == QStringLiteral("int"))
        {
            return QStringLiteral("Integer");
        }
        return QStringLiteral("String");
    }

    bool SerialDevice::isPropertyReadOnly(const QString&, const QString& name)
    {
        const auto it = m_profile.properties.constFind(name);
        return isPreInitName(name) || it == m_profile.properties.cend() || it->set.isEmpty();
    }

    bool SerialDevice::isPropertyPreInit(const QString&, const QString& name)
    {
        return isPreInitName(name);
    }

    QStringList SerialDevice::getAllowedPropertyValues(const QString&, const QString& name)
    {
        const auto it = m_profile.properties.constFind(name);
        if (it == m_profile.properties.cend())
        {
            return {};
        }
        if (it->type == QStringLiteral("action"))
        {
            return {QStringLiteral("Run")};
        }
        QStringList names;
        for (const auto& value : it->values)
        {
            names.append(value.first);
        }
        return names;
    }

    bool SerialDevice::hasPropertyLimits(const QString&, const QString& name)
    {
        const auto it = m_profile.properties.constFind(name);
        return it != m_profile.properties.cend() && it->min && it->max;
    }

    double SerialDevice::getPropertyLowerLimit(const QString&, const QString& name)
    {
        const auto it = m_profile.properties.constFind(name);
        return it == m_profile.properties.cend() ? 0.0 : it->min.value_or(0.0);
    }

    double SerialDevice::getPropertyUpperLimit(const QString&, const QString& name)
    {
        const auto it = m_profile.properties.constFind(name);
        return it == m_profile.properties.cend() ? 0.0 : it->max.value_or(0.0);
    }

    bool SerialDevice::readValue(const QString& name, QString* value, QString* errorMessage) const
    {
        const auto it = m_profile.properties.constFind(name);
        if (it == m_profile.properties.cend() || it->get.isEmpty())
        {
            *errorMessage = QStringLiteral("%1 cannot be read").arg(name);
            return false;
        }
        if (!query(*it, value, errorMessage))
        {
            return false;
        }
        m_cache.insert(name, *value);
        return true;
    }

    bool SerialDevice::setValue(const QString& name, const QString& value, QString* errorMessage)
    {
        const auto it = m_profile.properties.constFind(name);
        if (it == m_profile.properties.cend() || it->set.isEmpty())
        {
            *errorMessage = QStringLiteral("%1 cannot be set").arg(name);
            return false;
        }
        const SerialProperty& property = *it;

        // The command carries the token of the value, so an enum value goes out as its token
        QString token = value;
        if (property.type == QStringLiteral("enum"))
        {
            token.clear();
            for (const auto& allowed : property.values)
            {
                if (allowed.first == value)
                {
                    token = allowed.second;
                }
            }
            if (token.isEmpty())
            {
                *errorMessage = QStringLiteral("%1 is not a value of %2").arg(value, name);
                return false;
            }
        }
        else if (property.type == QStringLiteral("float") || property.type == QStringLiteral("int"))
        {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || (property.min && number < *property.min) || (property.max && number > *property.max))
            {
                *errorMessage = QStringLiteral("%1 is not a number in the range of %2").arg(value, name);
                return false;
            }
            // QString::number wrote small values such as 5e-05, which instruments do not read, so
            // floats are sent in fixed notation without trailing zeros
            if (property.type == QStringLiteral("int"))
            {
                token = QString::number(std::llround(number));
            }
            else
            {
                token = QString::number(number, 'f', 9);
                token.remove(QRegularExpression(QStringLiteral("\\.?0+$")));
            }
        }

        QString command = property.set;
        command.replace(QStringLiteral("{value}"), token);
        QString reply;
        if (!exchange(command, &reply, errorMessage))
        {
            return false;
        }
        if (reply != m_profile.setReply)
        {
            *errorMessage = QStringLiteral("Reply \"%1\" to \"%2\" from %3 is not \"%4\"")
                                .arg(reply, command, m_cache.value(kPort), m_profile.setReply);
            return false;
        }
        m_cache.insert(name, value);
        return true;
    }

    // One command and its reply. The reply is the line after the echo of the command, and an ack
    // line follows it when the description has one.
    bool SerialDevice::exchange(const QString& command, QString* reply, QString* errorMessage) const
    {
        const QString portName = m_port.portName();
        m_port.clear(QSerialPort::Input);
        m_pending.clear();
        const QByteArray bytes = (command + m_profile.writeTerminator).toLatin1();
        if (m_port.write(bytes) != bytes.size() || !m_port.waitForBytesWritten(m_profile.answerTimeout))
        {
            *errorMessage = QStringLiteral("Cannot send \"%1\" to %2: %3")
                                .arg(command, portName, m_port.errorString());
            return false;
        }

        do
        {
            if (!readLine(reply))
            {
                *errorMessage = QStringLiteral("No reply to \"%1\" from %2 within %3 ms")
                                    .arg(command, portName)
                                    .arg(m_profile.answerTimeout);
                return false;
            }
        } while (*reply == command);
        if (isErrorLine(*reply))
        {
            *errorMessage = QStringLiteral("Error reply \"%1\" to \"%2\" from %3")
                                .arg(*reply, command, portName);
            return false;
        }

        if (!m_profile.ack.isEmpty())
        {
            QString ack;
            if (!readLine(&ack))
            {
                *errorMessage = QStringLiteral("No ack to \"%1\" from %2 within %3 ms")
                                    .arg(command, portName)
                                    .arg(m_profile.answerTimeout);
                return false;
            }
            // The error pattern was checked only on the reply line, so an error in place of the ack
            // was reported as a wrong ack instead of the device's error
            if (isErrorLine(ack))
            {
                *errorMessage = QStringLiteral("Error reply \"%1\" to \"%2\" from %3")
                                    .arg(ack, command, portName);
                return false;
            }
            if (ack != m_profile.ack)
            {
                *errorMessage = QStringLiteral("Reply \"%1\" to \"%2\" from %3 is not \"%4\"")
                                    .arg(ack, command, portName, m_profile.ack);
                return false;
            }
        }
        return true;
    }

    // Reads up to the read terminator, and waits no longer than the answer timeout in all
    bool SerialDevice::readLine(QString* line) const
    {
        const QByteArray terminator = m_profile.readTerminator.toLatin1();
        QElapsedTimer timer;
        timer.start();
        qsizetype end = m_pending.indexOf(terminator);
        while (end < 0)
        {
            const qint64 remaining = m_profile.answerTimeout - timer.elapsed();
            if (remaining <= 0 || !m_port.waitForReadyRead(static_cast<int>(remaining)))
            {
                return false;
            }
            m_pending += m_port.readAll();
            end = m_pending.indexOf(terminator);
        }
        *line = QString::fromLatin1(m_pending.first(end));
        m_pending.remove(0, end + terminator.size());
        return true;
    }

    bool SerialDevice::isErrorLine(const QString& line) const
    {
        return !m_profile.errorPattern.isEmpty() && QRegularExpression(m_profile.errorPattern).match(line).hasMatch();
    }

    bool SerialDevice::query(const SerialProperty& property, QString* value, QString* errorMessage) const
    {
        QString reply;
        if (!exchange(property.get, &reply, errorMessage))
        {
            return false;
        }
        if (!property.parse.isEmpty())
        {
            const QRegularExpressionMatch match = QRegularExpression(property.parse).match(reply);
            if (!match.hasMatch())
            {
                *errorMessage = QStringLiteral("Unexpected reply \"%1\" to \"%2\" from %3")
                                    .arg(reply, property.get, m_cache.value(kPort));
                return false;
            }
            reply = match.captured(1);
        }
        if (property.type == QStringLiteral("enum"))
        {
            // The device answers with a token, which the description names
            for (const auto& allowed : property.values)
            {
                if (allowed.second == reply)
                {
                    *value = allowed.first;
                    return true;
                }
            }
            *errorMessage = QStringLiteral("Unexpected reply \"%1\" to \"%2\" from %3")
                                .arg(reply, property.get, m_cache.value(kPort));
            return false;
        }
        *value = reply;
        return true;
    }

    bool SerialShutterDevice::isShutterOpen(const QString&, bool& open, QString* errorMessage) const
    {
        QString unused;
        QString value;
        const SerialShutter& shutter = *profile().shutter;
        if (!readValue(shutter.property, &value, errorMessage ? errorMessage : &unused))
        {
            return false;
        }
        open = value == shutter.open;
        return true;
    }

    bool SerialShutterDevice::setShutterOpen(const QString&, bool open, QString* errorMessage)
    {
        QString unused;
        const SerialShutter& shutter = *profile().shutter;
        return setValue(shutter.property, open ? shutter.open : shutter.closed, errorMessage ? errorMessage : &unused);
    }
}
