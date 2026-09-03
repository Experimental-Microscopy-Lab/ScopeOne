#include "scopeone/ToolPlugin.h"

#include <QLabel>
#include <QObject>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
    class ExampleToolPlugin final : public QObject,
                                    public scopeone::ui::ScopeOneToolPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneToolPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::ui::ScopeOneToolPlugin)

    public:
        QList<scopeone::ui::ToolDescriptor> tools() const override
        {
            return {{QStringLiteral("example.tool.window"),
                     QStringLiteral("Example Tool"),
                     QStringLiteral("Examples")}};
        }

        QWidget* createTool(const QString& toolId,
                            scopeone::ui::ScopeOneToolContext& context,
                            QWidget* parent) override
        {
            if (toolId != QStringLiteral("example.tool.window"))
            {
                return nullptr;
            }
            auto* window = new QWidget(parent, Qt::Window);
            window->setWindowTitle(QStringLiteral("Example Tool"));
            auto* layout = new QVBoxLayout(window);
            layout->addWidget(new QLabel(
                QStringLiteral("Active layer: %1").arg(context.currentLayerKey()), window));
            return window;
        }
    };
}

#include "ExampleToolPlugin.moc"
