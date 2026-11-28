#pragma once

#include "internal/ProcessingModule.h"

#include <QStringList>

namespace scopeone::core::internal
{
    class ProcessingModuleRegistry;

    // Registers the modules of one script and returns their ids, or nothing and the error when the
    // script cannot be read or a module id is taken. Each module of a pipeline then talks to its
    // own Python process, one frame at a time.
    QStringList registerPythonScript(ProcessingModuleRegistry& registry,
                                     const QString& scriptPath,
                                     QString& error);
}
