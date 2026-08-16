#pragma once

#include "scopeone/HardwareTypes.h"

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT ClockService
    {
    public:
        ClockStamp now() const;
    };
}
