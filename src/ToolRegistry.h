#pragma once

#include "tools/ToolContext.h"

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QMenu;
class QWidget;

namespace scopeone::ui
{
    // The Tools menu and the open tool windows, for the tools registered in src/tools
    class ToolRegistry
    {
    public:
        explicit ToolRegistry(ToolContext& context);
        ~ToolRegistry();

        void populateMenu(QMenu* menu, QWidget* parent);
        void setEnabled(bool enabled);
        void updateActions();

    private:
        struct Entry;
        void openTool(const QString& toolId, QWidget* parent);

        ToolContext& m_context;
        std::vector<std::unique_ptr<Entry>> m_entries;
        bool m_enabled{true};
    };
}
