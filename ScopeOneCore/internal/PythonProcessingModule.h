#pragma once

#include "internal/ProcessingModule.h"

#include <QStringList>

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry;

    // Registers the modules of every script in the python folder of a processing
    // directory. Each module of a pipeline then talks to its own Python process, one frame at a
    // time, over stdin and stdout. Returns the scripts and modules that could not be registered.
    QStringList registerPythonProcessingModules(ProcessingModuleRegistry& registry,
                                                const QString& processingDirectory);
}
