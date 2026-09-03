#include "DaqControlWidget.h"
#include "SignalMonitorWidget.h"

#include "scopeone/ToolPlugin.h"

#include <QObject>

namespace scopeone::plugins
{
    class ScanningDaqToolPlugin final : public QObject,
                                        public scopeone::ui::ScopeOneToolPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneToolPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::ui::ScopeOneToolPlugin)

    public:
        QList<scopeone::ui::ToolDescriptor> tools() const override
        {
            return {{QStringLiteral("scopeone.daq_control"),
                     QStringLiteral("Scanning and DAQ Control"),
                     QStringLiteral("Acquisition"),
                     scopeone::ui::ToolWindowMode::ModelessSingleton,
                     false},
                    {QStringLiteral("scopeone.signal_monitor"),
                     QStringLiteral("Signal Monitor"),
                     QStringLiteral("Acquisition"),
                     scopeone::ui::ToolWindowMode::ModelessSingleton,
                     false}};
        }

        QWidget* createTool(const QString& toolId,
                            scopeone::ui::ScopeOneToolContext& context,
                            QWidget* parent) override
        {
            if (toolId == QStringLiteral("scopeone.daq_control"))
            {
                return new DaqControlWidget(&context.core(), parent);
            }
            if (toolId == QStringLiteral("scopeone.signal_monitor"))
            {
                return new SignalMonitorWidget(&context.core(), context, parent);
            }
            return nullptr;
        }
    };
}

#include "ScanningDaqToolPlugin.moc"
