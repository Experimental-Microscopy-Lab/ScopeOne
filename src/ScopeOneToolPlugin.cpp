#include "ScopeOneToolPlugin.h"
#include "scopeone/PluginManifest.h"

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMenu>
#include <QPluginLoader>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace scopeone::ui
{
    struct ToolRegistry::Entry
    {
        ToolDescriptor descriptor;
        Factory factory;
        QAction* action{nullptr};
        QPointer<QWidget> instance;
    };

    ToolRegistry::ToolRegistry(ScopeOneToolContext& context)
        : m_context(context)
    {
    }

    ToolRegistry::~ToolRegistry()
    {
        for (const auto& entry : m_entries)
        {
            delete entry->instance;
        }
    }

    bool ToolRegistry::registerTool(const ToolDescriptor& descriptor, Factory factory)
    {
        const QString id = descriptor.id.trimmed();
        for (const auto& entry : m_entries)
        {
            if (entry->descriptor.id == id)
            {
                return false;
            }
        }
        if (id.isEmpty() || descriptor.name.trimmed().isEmpty() || !factory)
        {
            return false;
        }

        auto entry = std::make_unique<Entry>();
        entry->descriptor = descriptor;
        entry->descriptor.id = id;
        entry->factory = std::move(factory);
        m_entries.push_back(std::move(entry));
        return true;
    }

    QStringList ToolRegistry::loadPlugins(const QString& directoryPath)
    {
        QStringList errors;
        const QDir directory(directoryPath);
        for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
        {
            if (!QLibrary::isLibrary(file.absoluteFilePath()))
            {
                continue;
            }
            auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
            scopeone::core::PluginManifest manifest;
            QString manifestError;
            if (!scopeone::core::parsePluginManifest(
                    loader->metaData().value(QStringLiteral("MetaData")).toObject(),
                    scopeone::core::PluginKind::Tool,
                    manifest,
                    &manifestError))
            {
                errors.append(QStringLiteral("%1: %2").arg(file.fileName(), manifestError));
                continue;
            }
            auto* plugin = qobject_cast<ScopeOneToolPlugin*>(loader->instance());
            if (!plugin)
            {
                errors.append(QStringLiteral("%1: %2").arg(file.fileName(), loader->errorString()));
                continue;
            }

            const QList<ToolDescriptor> descriptors = plugin->tools();
            QSet<QString> ids;
            bool valid = !descriptors.isEmpty();
            for (const ToolDescriptor& descriptor : descriptors)
            {
                const QString id = descriptor.id.trimmed();
                if (id.isEmpty()
                    || descriptor.name.trimmed().isEmpty()
                    || ids.contains(id))
                {
                    valid = false;
                    break;
                }
                ids.insert(id);
            }
            if (!valid)
            {
                errors.append(QStringLiteral("%1: invalid or duplicate tool id").arg(file.fileName()));
                loader->unload();
                continue;
            }

            const size_t entryCount = m_entries.size();
            for (const ToolDescriptor& descriptor : descriptors)
            {
                const QString id = descriptor.id.trimmed();
                if (!registerTool(descriptor, [plugin, id](ScopeOneToolContext& context,
                                                            QWidget* parent)
                {
                    return plugin->createTool(id, context, parent);
                }))
                {
                    m_entries.resize(entryCount);
                    errors.append(QStringLiteral("%1: tool id conflicts with an existing tool")
                                      .arg(file.fileName()));
                    loader->unload();
                    break;
                }
            }
            if (m_entries.size() == entryCount)
            {
                continue;
            }
            m_pluginLoaders.push_back(std::move(loader));
        }
        return errors;
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
                    target = menu->addMenu(category);
                    categories.insert(category, target);
                }
            }
            entry->action = target->addAction(entry->descriptor.name);
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
            tool->setWindowFlag(Qt::Window, true);
            tool->setAttribute(Qt::WA_DeleteOnClose);
            entry->instance = tool;
            tool->show();
            QTimer::singleShot(0, tool, [tool, parent]()
            {
                QWidget* host = parent ? parent->window() : nullptr;
                if (!host)
                {
                    return;
                }
                const QPoint position = host->frameGeometry().center()
                                      - tool->frameGeometry().center();
                tool->move(position);
            });
        }
    }
}
