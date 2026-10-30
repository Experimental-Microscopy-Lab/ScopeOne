#include "internal/ParticleAnalysis.h"

#include "internal/FrameBufferUtils.h"
#include "scopeone/ImageOps.h"

namespace scopeone::core::internal
{
    bool detectParticles(const ImageFrame& frame,
                         int threshold,
                         int minArea,
                         int maxArea,
                         ScopeOneCore::ParticleDetectionResult& result,
                         int maxParticles)
    {
        result = ScopeOneCore::ParticleDetectionResult{};
        if (!frame.isValid()
            || (!frame.isMono8() && !frame.isMono16())
            || minArea <= 0
            || maxArea < minArea
            || maxParticles <= 0)
        {
            return false;
        }

        // The same particles and measurements as Analyze Particles in the menus
        const ImageFrame mask = imageops::thresholded(
            frame, qBound(0, threshold, frame.maxValue()), frame.maxValue());
        QList<imageops::Particle> accepted;
        for (const imageops::Particle& found : imageops::findParticles(mask))
        {
            const int area = static_cast<int>(found.pixels.size());
            if (area < minArea || area > maxArea)
            {
                continue;
            }
            if (result.particles.size() >= maxParticles)
            {
                result.truncated = true;
                continue;
            }

            const imageops::Measurement measurement = imageops::measureParticle(frame, found, 0.0, false);
            ScopeOneCore::ParticleMeasurement particle;
            particle.area = area;
            particle.bounds = found.bounds;
            particle.centroid = QPointF(measurement.centroidX, measurement.centroidY);
            result.particles.append(particle);
            accepted.append(found);
        }

        result.mask = imageops::particleImage(accepted, frame.width, frame.height, false);
        copyFrameMetadata(frame, result.mask);
        return result.mask.isValid();
    }
}
