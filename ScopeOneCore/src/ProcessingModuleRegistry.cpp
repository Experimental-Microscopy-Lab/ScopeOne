#include "internal/ProcessingModuleRegistry.h"

#include <QSet>

#include <algorithm>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        bool validDescriptor(const ProcessingModuleDescriptor& descriptor)
        {
            QSet<QString> parameterKeys;
            for (const ProcessingParameterDescriptor& parameter : descriptor.parameters)
            {
                const QString key = parameter.key.trimmed();
                if (key.isEmpty()
                    || parameter.name.trimmed().isEmpty()
                    || parameterKeys.contains(key))
                {
                    return false;
                }
                parameterKeys.insert(key);
            }
            return !descriptor.id.trimmed().isEmpty()
                   && !descriptor.name.trimmed().isEmpty();
        }

        // A function-local list, so the registrations of other source files can add to it during
        // static initialization in any order
        std::vector<ProcessingModuleRegistration>& registrations()
        {
            static std::vector<ProcessingModuleRegistration> list;
            return list;
        }
    }

    bool addProcessingModuleRegistration(ProcessingModuleRegistration registration)
    {
        registrations().push_back(registration);
        return true;
    }

    ProcessingModuleRegistry::ProcessingModuleRegistry()
    {
        for (const ProcessingModuleRegistration registration : registrations())
        {
            registration(*this);
        }
    }

    ProcessingModuleRegistry::~ProcessingModuleRegistry() = default;

    bool ProcessingModuleRegistry::registerModule(const ProcessingModuleDescriptor& descriptor,
                                                  Factory factory)
    {
        const QString id = descriptor.id.trimmed();
        if (!validDescriptor(descriptor) || !factory || m_entries.contains(id))
        {
            return false;
        }

        ProcessingModuleDescriptor normalized = descriptor;
        normalized.id = id;
        m_entries.insert(id, {std::move(normalized), std::move(factory)});
        return true;
    }

    void ProcessingModuleRegistry::registerBuiltIn(const ProcessingModuleDescriptor& descriptor,
                                                   Factory factory)
    {
        // Builds are Release, where Q_ASSERT does nothing, and a module with a repeated id was
        // dropped without a word. Stopping at start up shows it to whoever added the module.
        if (!registerModule(descriptor, std::move(factory)))
        {
            qFatal("Processing module %s is registered twice or has an invalid descriptor",
                   qPrintable(descriptor.id));
        }
    }

    QList<ProcessingModuleDescriptor> ProcessingModuleRegistry::descriptors() const
    {
        QList<ProcessingModuleDescriptor> result;
        result.reserve(m_entries.size());
        for (const Entry& entry : m_entries)
        {
            result.append(entry.descriptor);
        }
        std::sort(result.begin(), result.end(),
                  [](const ProcessingModuleDescriptor& a, const ProcessingModuleDescriptor& b)
                  {
                      return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                  });
        return result;
    }

    ProcessingModuleDescriptor ProcessingModuleRegistry::descriptor(const QString& moduleId) const
    {
        return m_entries.value(moduleId.trimmed()).descriptor;
    }

    std::unique_ptr<ProcessingModule> ProcessingModuleRegistry::create(const QString& moduleId) const
    {
        const auto it = m_entries.constFind(moduleId.trimmed());
        if (it == m_entries.constEnd())
        {
            return {};
        }
        std::unique_ptr<ProcessingModule> module = it->factory();
        return module && module->id() == it->descriptor.id ? std::move(module) : nullptr;
    }
}
