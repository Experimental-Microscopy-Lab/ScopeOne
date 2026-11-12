// The built-in driver of serial devices. Each description file in the drivers folder is a device
// of this driver, and the file says which commands the device takes.
#include "SerialDevice.h"

#include "scopeone/DriverSDK.h"

#include <QObject>
#include <QSerialPortInfo>

namespace serial_driver
{
    class SerialDriver final : public QObject, public Driver
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
        Q_INTERFACES(scopeone::core::Driver)

    public:
        QString driverId() const override
        {
            return QStringLiteral("scopeone.serial");
        }

        // The devices are the description files, so the name is checked when the device is created
        QStringList deviceNames() const override
        {
            return {};
        }

        // The baud rate and answer timeout default to those of the description file
        QList<PreInitProperty> preInitProperties(const QString& deviceName, const QVariantMap& values) const override
        {
            QStringList ports;
            for (const auto& port : QSerialPortInfo::availablePorts()) ports.append(port.portName());
            QString unused;
            const SerialProfile profile = loadSerialProfile(
                values.value(QStringLiteral("Description File")).toString(), deviceName, &unused).value_or(SerialProfile{});
            return {{QStringLiteral("Port"), {}, ports},
                    {QStringLiteral("BaudRate"), QString::number(profile.baudRate), {}},
                    {QStringLiteral("AnswerTimeout"), QString::number(profile.answerTimeout), {}}};
        }

        DevicePtr createDevice(const QString& name, const QVariantMap& preInitProperties, QString* errorMessage) override
        {
            QString unused;
            if (!errorMessage)
            {
                errorMessage = &unused;
            }
            return createSerialDevice(name, preInitProperties, errorMessage);
        }
    };
}

#include "SerialDriver.moc"
