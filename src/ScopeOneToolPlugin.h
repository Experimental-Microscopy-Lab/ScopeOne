#pragma once

#include "scopeone/ToolPlugin.h"

#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QMenu;
class QPluginLoader;
class QWidget;

namespace scopeone::ui
{
    class ToolRegistry
    {
    public:
        using Factory = std::function<QWidget*(ScopeOneToolContext&, QWidget*)>;

        explicit ToolRegistry(ScopeOneToolContext& context);
        ~ToolRegistry();

        bool registerTool(const ToolDescriptor& descriptor, Factory factory);
        QStringList loadPlugins(const QString& directoryPath);
        void populateMenu(QMenu* menu, QWidget* parent);
        void setEnabled(bool enabled);
        void updateActions();

    private:
        struct Entry;
        void openTool(const QString& toolId, QWidget* parent);

        ScopeOneToolContext& m_context;
        std::vector<std::unique_ptr<Entry>> m_entries;
        std::vector<std::unique_ptr<QPluginLoader>> m_pluginLoaders;
        bool m_enabled{true};
    };
}
