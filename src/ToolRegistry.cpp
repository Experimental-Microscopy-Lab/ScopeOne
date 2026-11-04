#include "ToolRegistry.h"

#include "scopeone/ScopeOneCore.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QMenu>
#include <QPointer>
#include <QTimer>

namespace scopeone::ui
{
    struct ToolRegistry::Entry
    {
        ToolDescriptor descriptor;
        ToolFactory factory;
        QAction* action{nullptr};
        QPointer<QWidget> instance;
    };

    ToolRegistry::ToolRegistry(ScopeOneToolContext& context)
        : m_context(context)
    {
        for (ToolDefinition& tool : registeredTools())
        {
            auto entry = std::make_unique<Entry>();
            entry->descriptor = std::move(tool.descriptor);
            entry->factory = std::move(tool.create);
            m_entries.push_back(std::move(entry));
        }
    }

    ToolRegistry::~ToolRegistry()
    {
        for (const auto& entry : m_entries)
        {
            delete entry->instance;
        }
    }

    void ToolRegistry::populateMenu(QMenu* menu, QWidget* parent)
    {
        QHash<QString, QMenu*> categories;
        for (const auto& entry : m_entries)
        {
            QMenu* target = menu;
            const QString category = entry->descriptor.category.trimmed();
            if (!category.isEmpty())
            {
                target = categories.value(category);
                if (!target)
                {
                    target = menu->addMenu(QCoreApplication::translate("Tools", category.toUtf8().constData()));
                    categories.insert(category, target);
                }
            }
            entry->action = target->addAction(
                QCoreApplication::translate("Tools", entry->descriptor.name.toUtf8().constData()));
            const QString id = entry->descriptor.id;
            QObject::connect(entry->action, &QAction::triggered, parent,
                             [this, id, parent]() { openTool(id, parent); });
        }
        updateActions();
    }

    void ToolRegistry::updateActions()
    {
        const bool hasCamera = !m_context.core().cameraIds().isEmpty();
        for (const auto& entry : m_entries)
        {
            if (entry->action)
            {
                entry->action->setEnabled(
                    m_enabled && (!entry->descriptor.requiresCamera || hasCamera));
            }
        }
    }

    void ToolRegistry::setEnabled(bool enabled)
    {
        m_enabled = enabled;
        for (const auto& entry : m_entries)
        {
            if (entry->instance)
            {
                entry->instance->setEnabled(enabled);
            }
        }
        updateActions();
    }

    void ToolRegistry::openTool(const QString& toolId, QWidget* parent)
    {
        const auto centerTool = [parent](QWidget* tool)
        {
            QWidget* host = parent->window();
            const QPoint position = host->frameGeometry().center()
                                  - tool->frameGeometry().center();
            tool->move(position);
        };

        for (const auto& entry : m_entries)
        {
            if (entry->descriptor.id != toolId)
            {
                continue;
            }
            if (entry->descriptor.windowMode == ToolWindowMode::ModelessSingleton && entry->instance)
            {
                entry->instance->show();
                entry->instance->raise();
                entry->instance->activateWindow();
                return;
            }

            QWidget* tool = entry->factory(m_context, parent);
            if (!tool)
            {
                return;
            }
            if (entry->descriptor.windowMode == ToolWindowMode::Modal)
            {
                if (auto* dialog = qobject_cast<QDialog*>(tool))
                {
                    dialog->exec();
                }
                delete tool;
                return;
            }
            tool->setParent(nullptr, Qt::Window);
            tool->setAttribute(Qt::WA_DeleteOnClose);
            entry->instance = tool;
            tool->adjustSize();
            tool->show();
            tool->raise();
            tool->activateWindow();
            QTimer::singleShot(0, tool, [tool, centerTool]()
            {
                tool->adjustSize();
                centerTool(tool);
            });
        }
    }
}
