#include "internal/FFTModule.h"
#include "internal/FrameBufferUtils.h"

#include <cmath>
#include <numbers>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal {

namespace {

bool frameToGrayFloat(const ImageFrame& frame, cv::Mat& output)
{
    if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16())) {
        return false;
    }

    const int cvType = frame.isMono16() ? CV_16UC1 : CV_8UC1;
    cv::Mat input(frame.height,
                  frame.width,
                  cvType,
                  const_cast<char*>(frame.bytes.constData()),
                  frame.stride);
    input.convertTo(output, CV_32F);
    return true;
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

void fftShift(const cv::Mat& image, cv::Mat& shifted)
{
    shifted.create(image.size(), image.type());
    const int xOffset = image.cols / 2;
    const int yOffset = image.rows / 2;
    for (int y = 0; y < image.rows; ++y) {
        const float* srcRow = image.ptr<float>((y + yOffset) % image.rows);
        float* dstRow = shifted.ptr<float>(y);
        for (int x = 0; x < image.cols; ++x) {
            dstRow[x] = srcRow[(x + xOffset) % image.cols];
        }
    }
}

void magnitudeSpectrum(const cv::Mat* planes, cv::Mat& magnitude, cv::Mat& shifted)
{
    cv::magnitude(planes[0], planes[1], magnitude);
    magnitude += cv::Scalar::all(1.0);
    cv::log(magnitude, magnitude);
    fftShift(magnitude, shifted);
}

cv::Mat cropCenter(const cv::Mat& image, const cv::Size& size)
{
    const int x = (image.cols - size.width) / 2;
    const int y = (image.rows - size.height) / 2;
    return image(cv::Rect(x, y, size.width, size.height));
}

cv::Mat buildMask(const cv::Size& size,
                  double minFeatureSize,
                  double maxFeatureSize,
                  FFTModule::FilterKind filterKind)
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
                row[x] = (rsq * maxFeatureSize * maxFeatureSize > 1.0
                          && rsq * minFeatureSize * minFeatureSize < 1.0) ? 1.0f : 0.0f;
            } else {
                row[x] = static_cast<float>(
                    std::exp(-rsq * minFeatureSize * minFeatureSize / 2.0)
                    - std::exp(-rsq * maxFeatureSize * maxFeatureSize / 2.0));
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

const cv::Mat& FFTModule::maskForSize(const cv::Size& size)
{
    if (m_mask.empty()
        || m_maskSize != size
        || m_maskMinFeatureSize != m_minFeatureSize
        || m_maskMaxFeatureSize != m_maxFeatureSize
        || m_maskFilterKind != m_filterKind)
    {
        m_mask = buildMask(size, m_minFeatureSize, m_maxFeatureSize, m_filterKind);
        m_maskSize = size;
        m_maskMinFeatureSize = m_minFeatureSize;
        m_maskMaxFeatureSize = m_maxFeatureSize;
        m_maskFilterKind = m_filterKind;
    }
    return m_mask;
}

void FFTModule::invalidateMask()
{
    m_mask.release();
    m_maskSize = {};
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

        if (!frameToGrayFloat(workingFrame, m_grayFloat)) {
            out.frame = in.frame;
            out.error = "Failed to convert frame to grayscale";
            return false;
        }

        int optRows = cv::getOptimalDFTSize(m_grayFloat.rows);
        int optCols = cv::getOptimalDFTSize(m_grayFloat.cols);
        cv::copyMakeBorder(m_grayFloat,
                           m_padded,
                           0,
                           optRows - m_grayFloat.rows,
                           0,
                           optCols - m_grayFloat.cols,
                           cv::BORDER_CONSTANT,
                           0);

        cv::dft(m_padded, m_complex, cv::DFT_COMPLEX_OUTPUT);
        cv::split(m_complex, m_planes);

        if (m_outputMode == OutputMode::Spectrum || m_outputMode == OutputMode::BandpassSpectrum) {
            if (m_outputMode == OutputMode::BandpassSpectrum) {
                const cv::Mat& mask = maskForSize(m_padded.size());
                cv::multiply(m_planes[0], mask, m_planes[0]);
                cv::multiply(m_planes[1], mask, m_planes[1]);
            }

            magnitudeSpectrum(m_planes, m_spectrumMagnitude, m_shiftedSpectrum);
            cv::Mat spectrum = m_shiftedSpectrum;
            if (spectrum.size() != m_grayFloat.size()) {
                spectrum = cropCenter(spectrum, m_grayFloat.size());
            }
            out.frame = matToOutputFrame(spectrum, workingFrame, in.frame.cameraId);
            return true;
        }

        const cv::Mat& mask = maskForSize(m_padded.size());
        cv::multiply(m_planes[0], mask, m_planes[0]);
        cv::multiply(m_planes[1], mask, m_planes[1]);

        cv::merge(m_planes, 2, m_filteredComplex);
        cv::dft(m_filteredComplex, m_filtered, cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);

        const cv::Mat cropped = m_filtered(cv::Rect(0, 0, m_grayFloat.cols, m_grayFloat.rows));
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
    params["min_feature_size"] = m_minFeatureSize;
    params["max_feature_size"] = m_maxFeatureSize;
    params["filter_kind"] = static_cast<int>(m_filterKind);
    params["output_mode"] = static_cast<int>(m_outputMode);
    return params;
}

void FFTModule::setParameters(const QVariantMap& params)
{
    bool maskChanged = false;
    if (params.contains("min_feature_size")) {
        const double oldValue = m_minFeatureSize;
        m_minFeatureSize = qMax(0.0, params.value("min_feature_size").toDouble());
        if (m_minFeatureSize != oldValue) {
            maskChanged = true;
        }
    }
    if (params.contains("max_feature_size")) {
        const double oldValue = m_maxFeatureSize;
        m_maxFeatureSize = qMax(0.0, params.value("max_feature_size").toDouble());
        if (m_maxFeatureSize != oldValue) {
            maskChanged = true;
        }
    }
    if (m_minFeatureSize > m_maxFeatureSize) {
        std::swap(m_minFeatureSize, m_maxFeatureSize);
        maskChanged = true;
    }
    if (params.contains("filter_kind")) {
        const int filterKind = params.value("filter_kind").toInt();
        if (filterKind == 0 || filterKind == 1) {
            const auto newFilterKind = static_cast<FilterKind>(filterKind);
            if (m_filterKind != newFilterKind) {
                maskChanged = true;
            }
            m_filterKind = newFilterKind;
        }
    }
    if (params.contains("output_mode")) {
        const int outputMode = params.value("output_mode").toInt();
        if (outputMode >= 0 && outputMode <= 2) {
            m_outputMode = static_cast<OutputMode>(outputMode);
        }
    }
    if (maskChanged) {
        invalidateMask();
    }
}

} // namespace scopeone::core::internal
