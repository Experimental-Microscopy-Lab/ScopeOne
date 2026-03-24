#include "internal/GaussianBlurModule.h"
#include "internal/FrameBufferUtils.h"

#include <cstring>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal {

GaussianBlurModule::GaussianBlurModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool GaussianBlurModule::process(const ModuleInput& in, ModuleOutput& out)
{
    // Blur one mono frame with the current kernel
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame mono8Frame;
        if (!convertFrameToMono8(in.frame, mono8Frame)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        cv::Mat src(mono8Frame.height, mono8Frame.width, CV_8UC1,
                    mono8Frame.bytes.data(), mono8Frame.stride);
        cv::Mat blurred;
        cv::GaussianBlur(src, blurred, cv::Size(m_kernelSize, m_kernelSize), m_sigma, m_sigma);

        QByteArray bytes;
        bytes.resize(blurred.cols * blurred.rows);
        for (int y = 0; y < blurred.rows; ++y) {
            memcpy(bytes.data() + y * blurred.cols, blurred.ptr(y), static_cast<size_t>(blurred.cols));
        }

        out.frame = makeMono8Frame(in.frame.cameraId, blurred.cols, blurred.rows, std::move(bytes));
    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Gaussian blur failed: %1").arg(e.what());
        return false;
    }
    return true;
}

QVariantMap GaussianBlurModule::getParameters() const
{
    QVariantMap params;
    params["kernel_size"] = m_kernelSize;
    params["sigma"] = m_sigma;
    return params;
}

void GaussianBlurModule::setParameters(const QVariantMap& params)
{
    // Force a valid odd kernel size
    if (params.contains("kernel_size")) {
        int kernelSize = qMax(1, params.value("kernel_size").toInt());
        if ((kernelSize % 2) == 0) {
            ++kernelSize;
        }
        m_kernelSize = kernelSize;
    }
    if (params.contains("sigma")) {
        m_sigma = qMax(0.0, params.value("sigma").toDouble());
    }
}

} // namespace scopeone::core::internal
