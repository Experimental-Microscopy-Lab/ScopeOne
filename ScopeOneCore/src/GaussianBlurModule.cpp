#include "internal/GaussianBlurModule.h"
#include "internal/FrameBufferUtils.h"

#include <utility>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal
{
    // Creates an independent Gaussian blur runtime
    std::unique_ptr<ProcessingModule> GaussianBlurModule::createRuntime() const
    {
        auto module = std::make_unique<GaussianBlurModule>();
        module->setParameters(parameters());
        return module;
    }

    // Applies Gaussian blur to one mono frame
    ProcessingResult GaussianBlurModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return {{}, QStringLiteral("Invalid input")};
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth))
            {
                return {{}, QStringLiteral("Unsupported input frame")};
            }

            const int cvType = workingFrame.isMono16() ? CV_16UC1 : CV_8UC1;
            cv::Mat src(workingFrame.height, workingFrame.width, cvType,
                        workingFrame.bytes.data(), workingFrame.stride);
            cv::Mat blurred;
            cv::GaussianBlur(src, blurred, cv::Size(m_kernelSize, m_kernelSize), m_sigma, m_sigma);

            QByteArray bytes = copyMatBytes(blurred);
            return {makeFrameLike(workingFrame, blurred.cols, blurred.rows, std::move(bytes)), {}};
        }
        catch (const std::exception& e)
        {
            return {{}, QString("Gaussian blur failed: %1").arg(e.what())};
        }
    }

    // Returns the current blur parameters
    QVariantMap GaussianBlurModule::parameters() const
    {
        QVariantMap params;
        params["kernel_size"] = m_kernelSize;
        params["sigma"] = m_sigma;
        return params;
    }

    // Updates the blur kernel and sigma parameters
    void GaussianBlurModule::setParameters(const QVariantMap& params)
    {
        if (params.contains("kernel_size"))
        {
            int kernelSize = qMax(1, params.value("kernel_size").toInt());
            if ((kernelSize % 2) == 0)
            {
                ++kernelSize;
            }
            m_kernelSize = kernelSize;
        }
        if (params.contains("sigma"))
        {
            m_sigma = qMax(0.0, params.value("sigma").toDouble());
        }
    }
} // namespace scopeone::core::internal
