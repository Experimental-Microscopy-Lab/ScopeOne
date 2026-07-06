#include "internal/GaussianBlurModule.h"
#include "internal/FrameBufferUtils.h"

#include <utility>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal
{
    // Creates a Gaussian blur processing module
    GaussianBlurModule::GaussianBlurModule(QObject* parent)
        : ProcessingModule(parent)
    {
    }

    // Applies Gaussian blur to one mono frame
    bool GaussianBlurModule::process(const ModuleInput& in, ModuleOutput& out)
    {
        if (!in.frame.isValid())
        {
            out.frame = in.frame;
            out.error = "Invalid input";
            return false;
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth))
            {
                out.frame = in.frame;
                out.error = "Unsupported input frame";
                return false;
            }

            const int cvType = workingFrame.isMono16() ? CV_16UC1 : CV_8UC1;
            cv::Mat src(workingFrame.height, workingFrame.width, cvType,
                        workingFrame.bytes.data(), workingFrame.stride);
            cv::Mat blurred;
            cv::GaussianBlur(src, blurred, cv::Size(m_kernelSize, m_kernelSize), m_sigma, m_sigma);

            QByteArray bytes = copyMatBytes(blurred);
            out.frame = makeFrameLike(workingFrame, blurred.cols, blurred.rows, std::move(bytes));
        }
        catch (const std::exception& e)
        {
            out.frame = in.frame;
            out.error = QString("Gaussian blur failed: %1").arg(e.what());
            return false;
        }
        return true;
    }

    // Returns the current blur parameters
    QVariantMap GaussianBlurModule::getParameters() const
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
