#pragma once

#include "scopeone/ProcessingTypes.h"

#include <QHash>
#include <QStringList>
#include <functional>
#include <memory>

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

    private:
        struct Entry
        {
            ProcessingModuleDescriptor descriptor;
            Factory factory;
        };

        QHash<QString, Entry> m_entries;
        QStringList m_order;
    };
}
