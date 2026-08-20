#pragma once

#include "scopeone/ProcessingPlugin.h"

#include <QHash>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

class QPluginLoader;

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry
    {
    public:
        using Factory = std::function<std::unique_ptr<ProcessingModule>()>;

        ProcessingModuleRegistry();
        ~ProcessingModuleRegistry();

        bool registerModule(const ProcessingModuleDescriptor& descriptor, Factory factory);
        QList<ProcessingModuleDescriptor> descriptors() const;
        ProcessingModuleDescriptor descriptor(const QString& moduleId) const;
        std::unique_ptr<ProcessingModule> create(const QString& moduleId) const;
        QStringList loadPlugins(const QString& directoryPath);

    private:
        struct Entry
        {
            ProcessingModuleDescriptor descriptor;
            Factory factory;
        };

        QHash<QString, Entry> m_entries;
        QStringList m_order;
        std::vector<std::unique_ptr<QPluginLoader>> m_pluginLoaders;
    };
}
