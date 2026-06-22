#include "internal/ProcessingModule.h"

namespace scopeone::core::internal
{
    // Initialize the common QObject base for processing modules
    ProcessingModule::ProcessingModule(QObject* parent)
        : QObject(parent)
    {
    }
} // namespace scopeone::core::internal
