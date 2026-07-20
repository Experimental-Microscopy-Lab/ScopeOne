#pragma once

#include "scopeone/ScopeOneCore.h"

namespace scopeone::core::internal
{
    bool detectParticles(const ImageFrame& frame,
                         int threshold,
                         int minArea,
                         int maxArea,
                         ScopeOneCore::ParticleDetectionResult& result,
                         int maxParticles);
}
