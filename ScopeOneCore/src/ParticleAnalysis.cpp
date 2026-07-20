#include "internal/ParticleAnalysis.h"

#include "internal/FrameBufferUtils.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

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

        const int type = frame.isMono16() ? CV_16UC1 : CV_8UC1;
        const cv::Mat image(frame.height,
                            frame.width,
                            type,
                            const_cast<char*>(frame.bytes.constData()),
                            static_cast<size_t>(frame.stride));
        cv::Mat thresholdMask;
        cv::compare(image,
                    cv::Scalar(qBound(0, threshold, frame.maxValue())),
                    thresholdMask,
                    cv::CMP_GE);

        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int componentCount = cv::connectedComponentsWithStats(
            thresholdMask,
            labels,
            stats,
            centroids,
            8);
        std::vector<uchar> accepted(static_cast<size_t>(componentCount), 0);
        for (int label = 1; label < componentCount; ++label)
        {
            const int area = stats.at<int>(label, cv::CC_STAT_AREA);
            if (area < minArea || area > maxArea)
            {
                continue;
            }
            if (result.particles.size() >= maxParticles)
            {
                result.truncated = true;
                continue;
            }

            ScopeOneCore::ParticleMeasurement particle;
            particle.area = area;
            particle.bounds = QRect(stats.at<int>(label, cv::CC_STAT_LEFT),
                                    stats.at<int>(label, cv::CC_STAT_TOP),
                                    stats.at<int>(label, cv::CC_STAT_WIDTH),
                                    stats.at<int>(label, cv::CC_STAT_HEIGHT));
            particle.centroid = QPointF(centroids.at<double>(label, 0),
                                        centroids.at<double>(label, 1));
            result.particles.append(particle);
            accepted[static_cast<size_t>(label)] = 1;
        }

        cv::Mat filtered = cv::Mat::zeros(labels.size(), CV_8UC1);
        for (int y = 0; y < labels.rows; ++y)
        {
            const int* labelRow = labels.ptr<int>(y);
            uchar* outputRow = filtered.ptr<uchar>(y);
            for (int x = 0; x < labels.cols; ++x)
            {
                const int label = labelRow[x];
                if (label > 0 && accepted[static_cast<size_t>(label)] != 0)
                {
                    outputRow[x] = 255;
                }
            }
        }

        result.mask = makeMono8Frame(frame.cameraId,
                                     frame.width,
                                     frame.height,
                                     copyMatBytes(filtered));
        copyFrameMetadata(frame, result.mask);
        return result.mask.isValid();
    }
}
