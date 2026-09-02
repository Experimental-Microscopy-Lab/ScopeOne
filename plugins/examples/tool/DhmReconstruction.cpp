#include "DhmReconstruction.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <queue>

namespace
{
    using scopeone::core::ImageFrame;
    using scopeone::dhm::DhmParameters;

    constexpr float Pi = 3.14159265358979323846f;
    constexpr float TwoPi = 2.0f * Pi;

    struct SpectrumData
    {
        cv::Mat shiftedSpectrum;
        cv::Mat logMagnitude;
        int roiWidth{0};
        int roiHeight{0};
    };

    cv::Mat inputAsFloat(const ImageFrame& input)
    {
        const int depth = input.isMono16() ? CV_16UC1 : CV_8UC1;
        const cv::Mat view(input.height,
                           input.width,
                           depth,
                           const_cast<char*>(input.bytes.constData()),
                           static_cast<size_t>(input.stride));
        cv::Mat result;
        view.convertTo(result, CV_32F);
        return result;
    }

    cv::Mat applyRoi(const cv::Mat& input, const DhmParameters& params)
    {
        if (params.roiMode == scopeone::dhm::DhmRoiMode::FullFrame)
        {
            return input.clone();
        }

        const int size = std::min(params.roiSize, std::min(input.cols, input.rows));
        const int x = (input.cols - size) / 2;
        const int y = (input.rows - size) / 2;
        return input(cv::Rect(x, y, size, size)).clone();
    }

    void fftShift(cv::Mat& input)
    {
        const int halfWidth = input.cols / 2;
        const int halfHeight = input.rows / 2;
        cv::Mat topLeft(input, cv::Rect(0, 0, halfWidth, halfHeight));
        cv::Mat topRight(input, cv::Rect(input.cols - halfWidth,
                                         0,
                                         halfWidth,
                                         halfHeight));
        cv::Mat bottomLeft(input, cv::Rect(0,
                                           input.rows - halfHeight,
                                           halfWidth,
                                           halfHeight));
        cv::Mat bottomRight(input, cv::Rect(input.cols - halfWidth,
                                            input.rows - halfHeight,
                                            halfWidth,
                                            halfHeight));

        cv::Mat temporary;
        topLeft.copyTo(temporary);
        bottomRight.copyTo(topLeft);
        temporary.copyTo(bottomRight);
        topRight.copyTo(temporary);
        bottomLeft.copyTo(topRight);
        temporary.copyTo(bottomLeft);
    }

    SpectrumData buildSpectrum(const ImageFrame& input, const DhmParameters& params)
    {
        SpectrumData result;
        const cv::Mat roi = applyRoi(inputAsFloat(input), params);
        result.roiWidth = roi.cols;
        result.roiHeight = roi.rows;

        const int fftWidth = cv::getOptimalDFTSize(roi.cols);
        const int fftHeight = cv::getOptimalDFTSize(roi.rows);
        cv::Mat padded;
        cv::copyMakeBorder(roi,
                           padded,
                           0,
                           fftHeight - roi.rows,
                           0,
                           fftWidth - roi.cols,
                           cv::BORDER_CONSTANT,
                           cv::Scalar::all(0));

        cv::dft(padded, result.shiftedSpectrum, cv::DFT_COMPLEX_OUTPUT);
        fftShift(result.shiftedSpectrum);

        cv::Mat planes[2];
        cv::split(result.shiftedSpectrum, planes);
        cv::magnitude(planes[0], planes[1], result.logMagnitude);
        result.logMagnitude += 1.0f;
        cv::log(result.logMagnitude, result.logMagnitude);
        return result;
    }

    ImageFrame normalizedFrame(const cv::Mat& values,
                                const ImageFrame& source,
                                const QString& cameraId)
    {
        cv::Mat normalized;
        cv::normalize(values, normalized, 0.0, 65535.0, cv::NORM_MINMAX, CV_16U);
        normalized = normalized.clone();

        ImageFrame frame;
        frame.cameraId = cameraId;
        frame.width = normalized.cols;
        frame.height = normalized.rows;
        frame.stride = static_cast<int>(normalized.step);
        frame.bitsPerSample = 16;
        frame.pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
        frame.frameIndex = source.frameIndex;
        frame.timestampNs = source.timestampNs;
        frame.bytes = QByteArray(reinterpret_cast<const char*>(normalized.data),
                                 static_cast<qsizetype>(normalized.total()
                                                        * normalized.elemSize()));
        return frame;
    }

    QPoint detectedOffset(const cv::Mat& logMagnitude)
    {
        const cv::Point point = scopeone::dhm::autoDetectSideband(logMagnitude);
        return {point.x - logMagnitude.cols / 2,
                point.y - logMagnitude.rows / 2};
    }

    cv::Mat circularMask(int width,
                         int height,
                         const cv::Point& center,
                         int radius,
                         bool softEdge,
                         double sigma)
    {
        cv::Mat mask = cv::Mat::zeros(height, width, CV_32F);
        cv::circle(mask, center, radius, cv::Scalar(1.0f), cv::FILLED, cv::LINE_AA);
        if (softEdge)
        {
            const int kernelSize = std::max(3, (static_cast<int>(std::ceil(sigma * 6.0)) | 1));
            cv::GaussianBlur(mask,
                             mask,
                             cv::Size(kernelSize, kernelSize),
                             sigma,
                             sigma,
                             cv::BORDER_REPLICATE);
            double maximum = 0.0;
            cv::minMaxLoc(mask, nullptr, &maximum);
            mask /= static_cast<float>(maximum);
        }
        return mask;
    }

    void circularShift(const cv::Mat& source, cv::Mat& destination, int shiftX, int shiftY)
    {
        destination.create(source.size(), source.type());
        for (int y = 0; y < source.rows; ++y)
        {
            const int sourceY = (y - shiftY + source.rows) % source.rows;
            for (int x = 0; x < source.cols; ++x)
            {
                const int sourceX = (x - shiftX + source.cols) % source.cols;
                destination.at<cv::Vec2f>(y, x) = source.at<cv::Vec2f>(sourceY, sourceX);
            }
        }
    }

    void applyMask(cv::Mat& spectrum, const cv::Mat& mask)
    {
        for (int y = 0; y < spectrum.rows; ++y)
        {
            auto* row = spectrum.ptr<cv::Vec2f>(y);
            const auto* maskRow = mask.ptr<float>(y);
            for (int x = 0; x < spectrum.cols; ++x)
            {
                row[x] *= maskRow[x];
            }
        }
    }

    void applyPropagation(cv::Mat& shiftedSpectrum,
                          double wavelength,
                          double pixelSize,
                          double z)
    {
        const double waveNumber = 2.0 * CV_PI / wavelength;
        const int centerX = shiftedSpectrum.cols / 2;
        const int centerY = shiftedSpectrum.rows / 2;
        for (int y = 0; y < shiftedSpectrum.rows; ++y)
        {
            const double fy = static_cast<double>(y - centerY)
                            / (pixelSize * shiftedSpectrum.rows);
            const double ky = 2.0 * CV_PI * fy;
            auto* row = shiftedSpectrum.ptr<cv::Vec2f>(y);
            for (int x = 0; x < shiftedSpectrum.cols; ++x)
            {
                const double fx = static_cast<double>(x - centerX)
                                / (pixelSize * shiftedSpectrum.cols);
                const double kx = 2.0 * CV_PI * fx;
                const double kz = std::sqrt(std::max(0.0,
                                                     waveNumber * waveNumber
                                                     - kx * kx
                                                     - ky * ky));
                const float real = static_cast<float>(std::cos(kz * z));
                const float imaginary = static_cast<float>(std::sin(kz * z));
                const cv::Vec2f value = row[x];
                row[x][0] = value[0] * real - value[1] * imaginary;
                row[x][1] = value[0] * imaginary + value[1] * real;
            }
        }
    }

    float wrappedDifference(float value, float reference)
    {
        float difference = value - reference;
        while (difference > Pi)
        {
            difference -= TwoPi;
        }
        while (difference < -Pi)
        {
            difference += TwoPi;
        }
        return difference;
    }

    cv::Mat unwrapPhase2D(const cv::Mat& wrapped,
                          const std::atomic_bool& cancel)
    {
        struct Node
        {
            float quality;
            int x;
            int y;

            bool operator<(const Node& other) const
            {
                return quality < other.quality;
            }
        };

        cv::Mat reliability = cv::Mat::zeros(wrapped.size(), CV_32F);
        for (int y = 0; y < wrapped.rows; ++y)
        {
            for (int x = 0; x < wrapped.cols; ++x)
            {
                const float horizontal = x + 1 < wrapped.cols
                                              ? std::abs(wrappedDifference(
                                                    wrapped.at<float>(y, x + 1),
                                                    wrapped.at<float>(y, x)))
                                              : 0.0f;
                const float vertical = y + 1 < wrapped.rows
                                            ? std::abs(wrappedDifference(
                                                  wrapped.at<float>(y + 1, x),
                                                  wrapped.at<float>(y, x)))
                                            : 0.0f;
                reliability.at<float>(y, x) = 1.0f / (horizontal + vertical + 1.0e-6f);
            }
        }

        double maximum = 0.0;
        cv::Point seed;
        cv::minMaxLoc(reliability, nullptr, &maximum, nullptr, &seed);

        cv::Mat unwrapped = cv::Mat::zeros(wrapped.size(), CV_32F);
        cv::Mat visited = cv::Mat::zeros(wrapped.size(), CV_8U);
        std::priority_queue<Node> queue;
        queue.push({reliability.at<float>(seed.y, seed.x), seed.x, seed.y});
        unwrapped.at<float>(seed.y, seed.x) = wrapped.at<float>(seed.y, seed.x);

        constexpr int dx[] = {1, -1, 0, 0};
        constexpr int dy[] = {0, 0, 1, -1};
        while (!queue.empty())
        {
            if (cancel)
            {
                return {};
            }

            const Node node = queue.top();
            queue.pop();
            if (visited.at<uchar>(node.y, node.x))
            {
                continue;
            }
            visited.at<uchar>(node.y, node.x) = 1;

            for (int direction = 0; direction < 4; ++direction)
            {
                const int nx = node.x + dx[direction];
                const int ny = node.y + dy[direction];
                if (nx < 0 || nx >= wrapped.cols || ny < 0 || ny >= wrapped.rows
                    || visited.at<uchar>(ny, nx))
                {
                    continue;
                }

                const float reference = unwrapped.at<float>(node.y, node.x);
                const float value = wrapped.at<float>(ny, nx);
                unwrapped.at<float>(ny, nx)
                    = value + TwoPi * std::round((reference - value) / TwoPi);
                queue.push({reliability.at<float>(ny, nx), nx, ny});
            }
        }
        return unwrapped;
    }

    void removeTilt(cv::Mat& phase)
    {
        const double centerX = (phase.cols - 1) / 2.0;
        const double centerY = (phase.rows - 1) / 2.0;
        const double scaleX = std::max(1.0, centerX);
        const double scaleY = std::max(1.0, centerY);

        double sums[3][3]{};
        double vector[3]{};
        for (int y = 0; y < phase.rows; ++y)
        {
            const double normalizedY = (y - centerY) / scaleY;
            for (int x = 0; x < phase.cols; ++x)
            {
                const double normalizedX = (x - centerX) / scaleX;
                const double terms[3] = {normalizedX, normalizedY, 1.0};
                const double value = phase.at<float>(y, x);
                for (int row = 0; row < 3; ++row)
                {
                    vector[row] += terms[row] * value;
                    for (int column = 0; column < 3; ++column)
                    {
                        sums[row][column] += terms[row] * terms[column];
                    }
                }
            }
        }

        cv::Mat matrix(3, 3, CV_64F, sums);
        cv::Mat rightHandSide(3, 1, CV_64F, vector);
        cv::Mat coefficients;
        cv::solve(matrix, rightHandSide, coefficients, cv::DECOMP_CHOLESKY);

        for (int y = 0; y < phase.rows; ++y)
        {
            const double normalizedY = (y - centerY) / scaleY;
            for (int x = 0; x < phase.cols; ++x)
            {
                const double normalizedX = (x - centerX) / scaleX;
                const double plane = coefficients.at<double>(0) * normalizedX
                                   + coefficients.at<double>(1) * normalizedY
                                   + coefficients.at<double>(2);
                phase.at<float>(y, x) -= static_cast<float>(plane);
            }
        }
    }
}

namespace scopeone::dhm
{
    cv::Point autoDetectSideband(const cv::Mat& logMagSpectrum)
    {
        cv::Mat blurred;
        cv::GaussianBlur(logMagSpectrum, blurred, cv::Size(0, 0), 2.0, 2.0);

        const cv::Point center(logMagSpectrum.cols / 2, logMagSpectrum.rows / 2);
        const int dcRadius = static_cast<int>(
            0.06 * std::min(logMagSpectrum.cols, logMagSpectrum.rows));
        cv::Mat mask = cv::Mat::zeros(logMagSpectrum.size(), CV_8U);
        cv::rectangle(mask,
                      cv::Rect(0, 0, logMagSpectrum.cols, std::max(1, center.y)),
                      cv::Scalar(255),
                      cv::FILLED);
        cv::circle(mask, center, dcRadius, cv::Scalar(0), cv::FILLED);

        cv::Point peak;
        cv::minMaxLoc(blurred, nullptr, nullptr, nullptr, &peak, mask);
        return peak;
    }

    std::pair<ImageFrame, QPoint> computeSpectrum(const ImageFrame& input,
                                                  const DhmParameters& params)
    {
        const SpectrumData spectrum = buildSpectrum(input, params);
        const ImageFrame frame = normalizedFrame(
            spectrum.logMagnitude, input, QStringLiteral("dhm.spectrum"));
        return {frame, detectedOffset(spectrum.logMagnitude)};
    }

    DhmResult reconstruct(const ImageFrame& input,
                          const DhmParameters& params,
                          const std::atomic_bool& cancel,
                          const std::function<void(int)>& progress)
    {
        const SpectrumData spectrum = buildSpectrum(input, params);
        const QPoint detected = detectedOffset(spectrum.logMagnitude);
        const QPoint selected = params.autoDetectSideband
                                    ? detected
                                    : QPoint(params.sidebandX, params.sidebandY);
        const int maximumX = spectrum.shiftedSpectrum.cols / 2 - 1;
        const int maximumY = spectrum.shiftedSpectrum.rows / 2 - 1;
        const int sidebandX = std::clamp(selected.x(), -maximumX, maximumX);
        const int sidebandY = std::clamp(selected.y(), -maximumY, maximumY);
        const int radius = std::clamp(
            params.radius,
            1,
            std::min(spectrum.shiftedSpectrum.cols, spectrum.shiftedSpectrum.rows) / 2);

        DhmResult result;
        result.detectedSidebandX = detected.x();
        result.detectedSidebandY = detected.y();
        result.spectrumFrame = normalizedFrame(
            spectrum.logMagnitude, input, QStringLiteral("dhm.spectrum"));
        progress(10);

        if (params.outputMode == DhmOutputMode::Spectrum)
        {
            result.outputFrame = result.spectrumFrame;
            progress(100);
            return result;
        }

        cv::Mat filtered = spectrum.shiftedSpectrum.clone();
        const cv::Point sideband(spectrum.shiftedSpectrum.cols / 2 + sidebandX,
                                 spectrum.shiftedSpectrum.rows / 2 + sidebandY);
        const cv::Mat mask = circularMask(spectrum.shiftedSpectrum.cols,
                                          spectrum.shiftedSpectrum.rows,
                                          sideband,
                                          radius,
                                          params.softEdge,
                                          params.softEdgeSigma);
        applyMask(filtered, mask);
        progress(30);

        cv::Mat demodulated;
        circularShift(filtered, demodulated, -sidebandX, -sidebandY);
        if (params.z != 0.0)
        {
            applyPropagation(demodulated,
                             params.wavelength,
                             params.pixelSize,
                             params.z);
        }
        progress(48);

        fftShift(demodulated);
        cv::Mat complexField;
        cv::idft(demodulated,
                 complexField,
                 cv::DFT_SCALE | cv::DFT_COMPLEX_OUTPUT);
        cv::Mat fieldRoi(complexField,
                         cv::Rect(0, 0, spectrum.roiWidth, spectrum.roiHeight));

        cv::Mat planes[2];
        cv::split(fieldRoi, planes);
        cv::Mat amplitude;
        cv::Mat phase;
        cv::magnitude(planes[0], planes[1], amplitude);
        cv::phase(planes[0], planes[1], phase, false);
        const cv::Mat wrappedPhase = phase.clone();
        progress(62);

        if (params.unwrapPhase)
        {
            phase = unwrapPhase2D(phase, cancel);
            if (phase.empty())
            {
                return {};
            }
        }
        if (cancel)
        {
            return {};
        }
        progress(78);

        if (params.removeTilt)
        {
            removeTilt(phase);
        }
        progress(88);

        switch (params.outputMode)
        {
        case DhmOutputMode::QuantitativePhase:
        {
            const cv::Mat quantitativePhase = phase * (params.wavelength / TwoPi);
            result.outputFrame = normalizedFrame(
                quantitativePhase, input, QStringLiteral("dhm.phase"));
            break;
        }
        case DhmOutputMode::WrappedPhase:
        {
            cv::Mat displayPhase = wrappedPhase.clone();
            for (int y = 0; y < displayPhase.rows; ++y)
            {
                for (int x = 0; x < displayPhase.cols; ++x)
                {
                    displayPhase.at<float>(y, x)
                        = (displayPhase.at<float>(y, x) + Pi) / TwoPi;
                }
            }
            result.outputFrame = normalizedFrame(
                displayPhase, input, QStringLiteral("dhm.wrapped_phase"));
            break;
        }
        case DhmOutputMode::Amplitude:
            result.outputFrame = normalizedFrame(
                amplitude, input, QStringLiteral("dhm.amplitude"));
            break;
        case DhmOutputMode::Spectrum:
            break;
        }

        progress(100);
        return result;
    }
}
