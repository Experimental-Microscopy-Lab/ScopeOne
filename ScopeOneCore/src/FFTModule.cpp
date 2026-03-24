#include "internal/FFTModule.h"
#include "internal/FrameBufferUtils.h"
#include <QDebug>
#include <cstring>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

namespace scopeone::core::internal {

namespace {

    static cv::Mat frameToGrayFloat(const ImageFrame& frame) {
        if (!frame.isValid() || !frame.isMono8()) return cv::Mat();
        cv::Mat m(frame.height, frame.width, CV_8UC1, const_cast<char*>(frame.bytes.constData()), frame.stride);
        cv::Mat mFloat;
        m.convertTo(mFloat, CV_32F);
        return mFloat;
    }

    static QByteArray matToMono8Bytes(const cv::Mat& input) {
        cv::Mat normalized;
        cv::normalize(input, normalized, 0.0, 255.0, cv::NORM_MINMAX, CV_8U);
        QByteArray bytes;
        bytes.resize(normalized.cols * normalized.rows);
        for (int y = 0; y < normalized.rows; ++y) {
            memcpy(bytes.data() + y * normalized.cols, normalized.ptr(y), static_cast<size_t>(normalized.cols));
        }
        return bytes;
    }

    cv::Mat buildMask(const cv::Size& size, double minWidth, double maxWidth, FFTModule::FilterKind filterKind)
    {
        // Build one bandpass mask in frequency space
        constexpr double kTwoPi = 6.2831853071795864769;
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
    // Apply one FFT bandpass filter to the input frame
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

        cv::Mat grayFloat = frameToGrayFloat(mono8Frame);
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
        out.frame = makeMono8Frame(in.frame.cameraId,
                                   cropped.cols,
                                   cropped.rows,
                                   matToMono8Bytes(cropped));

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
    // Keep the bandpass bounds ordered
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
