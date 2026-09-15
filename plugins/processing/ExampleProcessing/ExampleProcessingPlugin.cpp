#include "scopeone/ProcessingPlugin.h"

#include <QObject>

namespace
{
    class PassthroughModule final : public scopeone::core::ProcessingModule
    {
    public:
        QString id() const override { return QStringLiteral("example.passthrough"); }
        QString name() const override { return QStringLiteral("Example Passthrough"); }
        QVariantMap parameters() const override { return {}; }
        void setParameters(const QVariantMap&) override {}
        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override
        {
            return std::make_unique<PassthroughModule>();
        }
        scopeone::core::ProcessingResult process(const scopeone::core::ImageFrame& frame,
                                                  int) override
        {
            return {frame, {}};
        }
    };

    class ExampleProcessingPlugin final : public QObject,
                                          public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        QList<scopeone::core::ProcessingModuleDescriptor> processingModules() const override
        {
            return {{QStringLiteral("example.passthrough"),
                     QStringLiteral("Example Passthrough")}};
        }

        std::unique_ptr<scopeone::core::ProcessingModule> createProcessingModule(
            const QString& moduleId) override
        {
            return moduleId == QStringLiteral("example.passthrough")
                       ? std::make_unique<PassthroughModule>()
                       : nullptr;
        }
    };
}

#include "ExampleProcessingPlugin.moc"
