#pragma once

#include "scopeone/ProcessingTypes.h"

#include <QHash>
#include <functional>
#include <memory>

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry
    {
    public:
        using Factory = std::function<std::unique_ptr<ProcessingModule>()>;

        // Registers every module that its source file added with SCOPEONE_REGISTER_PROCESSING_MODULE
        ProcessingModuleRegistry();
        ~ProcessingModuleRegistry();

        bool registerModule(const ProcessingModuleDescriptor& descriptor, Factory factory);
        // For the modules built into Core, whose id and descriptor the program guarantees.
        // Stops with the module's id when one is registered twice or its descriptor is invalid.
        void registerBuiltIn(const ProcessingModuleDescriptor& descriptor, Factory factory);
        // Sorted by name, so a CPU module and its "(CUDA)" version are listed together
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
    };

    using ProcessingModuleRegistration = void (*)(ProcessingModuleRegistry& registry);

    // Called while Core is loaded, before any registry exists. Every registry runs the
    // registrations when it is built, so a descriptor that depends on the machine, such as the
    // ONNX providers, is made then and not at load time.
    bool addProcessingModuleRegistration(ProcessingModuleRegistration registration);
}

// Registers one module with its descriptor. Placed once at the end of the module's source file,
// so the registry and the processing list need no change.
#define SCOPEONE_REGISTER_PROCESSING_MODULE(Module, ...)                                         \
    namespace                                                                                    \
    {                                                                                            \
        [[maybe_unused]] const bool processingModuleRegistered =                                 \
            ::scopeone::core::internal::addProcessingModuleRegistration(                         \
                [](::scopeone::core::internal::ProcessingModuleRegistry& registry)               \
                {                                                                                \
                    registry.registerBuiltIn(                                                    \
                        ::scopeone::core::ProcessingModuleDescriptor{__VA_ARGS__},               \
                        []() -> std::unique_ptr<::scopeone::core::ProcessingModule>              \
                        { return std::make_unique<Module>(); });                                 \
                });                                                                              \
    }

