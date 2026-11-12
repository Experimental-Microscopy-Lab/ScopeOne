#pragma once

#include "scopeone/DriverSDK.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSerialPort>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace serial_driver
{
    using namespace scopeone::core;

    // A property as the description file gives it. The values pair the display name with the
    // token the device uses.
    struct SerialProperty
    {
        QString type;
        QString get;
        QString set;
        QRegularExpression parse;
        QList<QPair<QString, QString>> values;
        std::optional<double> min;
        std::optional<double> max;
        QString maxGet;
    };

    struct SerialShutter
    {
        QString property;
        QString open;
        QString closed;
    };

    // What a description file says about one serial device
    struct SerialProfile
    {
        QString name;
        QStringList devices;
        int baudRate{9600};
        QSerialPort::Parity parity{QSerialPort::NoParity};
        QSerialPort::StopBits stopBits{QSerialPort::OneStop};
        QSerialPort::FlowControl flowControl{QSerialPort::NoFlowControl};
        QString writeTerminator;
        QString readTerminator;
        QString setReply;
        QRegularExpression error;
        QString ack;
        int answerTimeout{500};
        QMap<QString, SerialProperty> properties;
        std::optional<SerialShutter> shutter;
    };

    // Reads a description file, and returns nothing with the reason in errorMessage
    std::optional<SerialProfile> loadSerialProfile(const QString& path,
                                                   const QString& deviceName,
                                                   QString* errorMessage);

    // Creates the device of a description file on the port in the pre-init properties
    DevicePtr createSerialDevice(const QString& name,
                                 const QVariantMap& preInitProperties,
                                 QString* errorMessage);

    // A device that talks over a serial port with the commands of its description file
    class SerialDevice : public Device
    {
    public:
        SerialDevice(SerialProfile profile, const QString& portName, const QString& descriptionPath);

        // Opens the port named in the cache and reads the maximums that the description takes from the device
        bool open(QString* errorMessage);

        HardwareProviderDescriptor descriptor() const override;
        QStringList listProperties(const QString& deviceId) override;
        QString getProperty(const QString& deviceId, const QString& name, bool fromCache = false) override;
        bool setProperty(const QString& deviceId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage = nullptr) override;
        QString getPropertyType(const QString& deviceId, const QString& name) override;
        bool isPropertyReadOnly(const QString& deviceId, const QString& name) override;
        bool isPropertyPreInit(const QString& deviceId, const QString& name) override;
        QStringList getAllowedPropertyValues(const QString& deviceId, const QString& name) override;
        bool hasPropertyLimits(const QString& deviceId, const QString& name) override;
        double getPropertyLowerLimit(const QString& deviceId, const QString& name) override;
        double getPropertyUpperLimit(const QString& deviceId, const QString& name) override;

    protected:
        const SerialProfile& profile() const { return m_profile; }
        // Reads a property from the device and caches the value
        bool readValue(const QString& name, QString* value, QString* errorMessage) const;
        bool setValue(const QString& name, const QString& value, QString* errorMessage);

    private:
        bool exchange(const QString& command, QString* reply, QString* errorMessage) const;
        bool readLine(QString* line) const;
        bool isErrorLine(const QString& line) const;
        bool query(const SerialProperty& property, QString* value, QString* errorMessage) const;

        SerialProfile m_profile;
        // The port and the cache are mutable so that the const shutter query can read the device
        mutable QSerialPort m_port;
        mutable QByteArray m_pending;
        mutable QHash<QString, QString> m_cache;
        // Whether the last exchange failed on an error reply of the device, not on the port
        mutable bool m_errorReply{false};
    };

    // The serial device of a description with a shutter, which adds the Shutter capability
    class SerialShutterDevice final : public SerialDevice, public Shutter
    {
    public:
        using SerialDevice::SerialDevice;

        bool isShutterOpen(const QString& deviceId, bool& open, QString* errorMessage) const override;
        bool setShutterOpen(const QString& deviceId, bool open, QString* errorMessage) override;
    };
}
