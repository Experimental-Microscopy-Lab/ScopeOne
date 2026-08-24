#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/SimulatorProvider.h"

#include <QObject>

namespace
{
    class ExampleHardwarePlugin final : public QObject,
                                        public scopeone::core::DriverHostProviderPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriverHostProviderPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::DriverHostProviderPlugin)

    public:
        QString providerId() const override
        {
            return QStringLiteral("example.hardware");
        }

        scopeone::core::HardwareProviderPtr createProvider(const QJsonObject& options,
                                                            QString* errorMessage) override
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            const int width = options.value(QStringLiteral("width")).toInt(512);
            const int height = options.value(QStringLiteral("height")).toInt(512);
            if (width <= 0 || height <= 0)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("width and height must be positive");
                }
                return {};
            }
            return std::make_shared<scopeone::core::SimulatorProvider>(
                QStringLiteral("camera.example"), width, height, providerId());
        }
    };
}

#include "ExampleHardwarePlugin.moc"
