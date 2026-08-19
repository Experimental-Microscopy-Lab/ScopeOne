#include <QObject>

#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/SimulatorProvider.h"

namespace scopeone::core
{
    class SimulatorProviderPlugin final : public QObject,
                                          public DriverHostProviderPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriverHostProviderPlugin_iid)
        Q_INTERFACES(scopeone::core::DriverHostProviderPlugin)

    public:
        QString providerId() const override
        {
            return QStringLiteral("simulator");
        }

        HardwareProviderPtr createProvider(const QJsonObject& options,
                                           QString* errorMessage) override
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            const QString deviceId = options.value(QStringLiteral("cameraId"))
                                         .toString(QStringLiteral("camera.simulator"))
                                         .trimmed();
            const int width = options.value(QStringLiteral("width")).toInt(512);
            const int height = options.value(QStringLiteral("height")).toInt(512);
            if (deviceId.isEmpty() || width <= 0 || height <= 0)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Invalid simulator options");
                }
                return {};
            }
            return std::make_shared<SimulatorProvider>(deviceId,
                                                       width,
                                                       height,
                                                       providerId());
        }
    };
}

#include "SimulatorProviderPlugin.moc"
