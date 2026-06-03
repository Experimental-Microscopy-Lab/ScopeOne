#include "internal/FFTModule.h"
#include "internal/FrameBufferUtils.h"

#include <numbers>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal {

namespace {

cv::Mat frameToGrayFloat(const ImageFrame& frame)
{
    if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16())) {
        return {};
    }

    const int cvType = frame.isMono16() ? CV_16UC1 : CV_8UC1;
    cv::Mat input(frame.height,
                  frame.width,
                  cvType,
                  const_cast<char*>(frame.bytes.constData()),
                  frame.stride);
    cv::Mat floatFrame;
    input.convertTo(floatFrame, CV_32F);
    return floatFrame;
}

ImageFrame matToOutputFrame(const cv::Mat& input,
                            const ImageFrame& reference,
                            const QString& cameraId)
{
    cv::Mat normalized;
    const int targetType = reference.isMono16() ? CV_16U : CV_8U;
    const double targetMax = static_cast<double>(reference.maxValue());
    cv::normalize(input, normalized, 0.0, targetMax, cv::NORM_MINMAX, targetType);
    QByteArray bytes = copyMatBytes(normalized);
    ImageFrame output = makeFrameLike(reference, normalized.cols, normalized.rows, std::move(bytes));
    output.cameraId = cameraId;
    return output;
}

cv::Mat buildMask(const cv::Size& size, double minWidth, double maxWidth, FFTModule::FilterKind filterKind)
{
    constexpr double kTwoPi = 2.0 * std::numbers::pi_v<double>;
    cv::Mat centered(size, CV_32F);
    for (int y = 0; y < size.height; ++y) {
        const double fy = (static_cast<double>(y) - size.height / 2.0) / static_cast<double>(size.height);
        float* row = centered.ptr<float>(y);
        for (int x = 0; x < size.width; ++x) {
            const double fx = (static_cast<double>(x) - size.width / 2.0) / static_cast<double>(size.width);
            const double rsq = (kTwoPi * fx) * (kTwoPi * fx) + (kTwoPi * fy) * (kTwoPi * fy);
            if (filterKind == FFTModule::FilterKind::Hard) {
                row[x] = (rsq * maxWidth * maxWidth > 1.0 && rsq * minWidth * minWidth < 1.0) ? 1.0f : 0.0f;
            } else {
                row[x] = static_cast<float>(
                    std::exp(-rsq * minWidth * minWidth / 2.0)
                    - std::exp(-rsq * maxWidth * maxWidth / 2.0));
            }
        }
    }

    cv::Mat mask(size, CV_32F);
    const int cx = size.width / 2;
    const int cy = size.height / 2;
    centered(cv::Rect(cx, cy, size.width - cx, size.height - cy)).copyTo(mask(cv::Rect(0, 0, size.width - cx, size.height - cy)));
    centered(cv::Rect(0, cy, cx, size.height - cy)).copyTo(mask(cv::Rect(size.width - cx, 0, cx, size.height - cy)));
    centered(cv::Rect(cx, 0, size.width - cx, cy)).copyTo(mask(cv::Rect(0, size.height - cy, size.width - cx, cy)));
    centered(cv::Rect(0, 0, cx, cy)).copyTo(mask(cv::Rect(size.width - cx, size.height - cy, cx, cy)));
    return mask;
}
}

FFTModule::FFTModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool FFTModule::process(const ModuleInput& in, ModuleOutput& out)
{
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame workingFrame;
        if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth)) {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        cv::Mat grayFloat = frameToGrayFloat(workingFrame);
        if (grayFloat.empty()) {
            out.frame = in.frame;
            out.error = "Failed to convert frame to grayscale";
            return false;
        }

        int optRows = cv::getOptimalDFTSize(grayFloat.rows);
        int optCols = cv::getOptimalDFTSize(grayFloat.cols);
        cv::Mat padded;
        cv::copyMakeBorder(grayFloat, padded, 0, optRows - grayFloat.rows, 0, optCols - grayFloat.cols, cv::BORDER_CONSTANT, 0);

        cv::Mat complex;
        cv::dft(padded, complex, cv::DFT_COMPLEX_OUTPUT);

        cv::Mat planes[2];
        cv::split(complex, planes);
        const cv::Mat mask = buildMask(padded.size(), m_minWidth, m_maxWidth, m_filterKind);
        planes[0] = planes[0].mul(mask);
        planes[1] = planes[1].mul(mask);

        cv::Mat filteredComplex;
        cv::merge(planes, 2, filteredComplex);
        cv::Mat filtered;
        cv::dft(filteredComplex, filtered, cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);

        const cv::Mat cropped = filtered(cv::Rect(0, 0, grayFloat.cols, grayFloat.rows)).clone();
        out.frame = matToOutputFrame(cropped, workingFrame, in.frame.cameraId);

    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("FFT processing failed: %1").arg(e.what());
        return false;
    }

    return true;
}

QVariantMap FFTModule::getParameters() const
{
    QVariantMap params;
    params["min_width"] = m_minWidth;
    params["max_width"] = m_maxWidth;
    params["filter_kind"] = static_cast<int>(m_filterKind);
    return params;
}

void FFTModule::setParameters(const QVariantMap& params)
{
    if (params.contains("min_width")) {
        m_minWidth = qMax(0.0, params.value("min_width").toDouble());
    }
    if (params.contains("max_width")) {
        m_maxWidth = qMax(0.0, params.value("max_width").toDouble());
    }
    if (m_minWidth > m_maxWidth) {
        std::swap(m_minWidth, m_maxWidth);
    }
    if (params.contains("filter_kind")) {
        const int filterKind = params.value("filter_kind").toInt();
        if (filterKind == 0 || filterKind == 1) {
            m_filterKind = static_cast<FilterKind>(filterKind);
        }
    }
}

} // namespace scopeone::core::internal
