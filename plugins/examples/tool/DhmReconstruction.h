#pragma once

#include "scopeone/ImageFrame.h"

#include <atomic>
#include <functional>

namespace scopeone::dhm
{
    scopeone::core::ImageFrame reconstructPhase(
        const scopeone::core::ImageFrame& input,
        int sidebandX,
        int sidebandY,
        int radius,
        const std::atomic_bool& cancel,
        const std::function<void(int)>& progress);
}
