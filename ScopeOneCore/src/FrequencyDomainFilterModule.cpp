#include "internal/FrequencyDomainFilterModule.h"

#include "internal/ProcessingModuleRegistry.h"
#include "internal/ProcessingParameters.h"

#include "internal/FrameBufferUtils.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>
#include <utility>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <pocketfft_hdronly.h>

namespace scopeone::core::internal
{
    namespace
    {
        ImageFrame outputFrame(const cv::Mat& input, const ImageFrame& reference)
        {
            const int type = reference.isMono16() ? CV_16U : CV_8U;
            QByteArray bytes = reference.isMono16()
                                   ? allocatePixelBytes<quint16>(input.cols, input.rows)
                                   : allocatePixelBytes<uchar>(input.cols, input.rows);
            if (bytes.isEmpty())
            {
                return {};
            }
            cv::Mat output(input.rows,
                           input.cols,
                           type,
                           bytes.data(),
                           input.cols * reference.bytesPerPixel());
            cv::normalize(input,
                          output,
                          0.0,
                          static_cast<double>(reference.maxValue()),
                          cv::NORM_MINMAX,
                          type);
            return makeFrameLike(reference, output.cols, output.rows, std::move(bytes));
        }

        void fftShift(const cv::Mat& input, cv::Mat& output)
        {
            output.create(input.size(), input.type());
            const int xOffset = input.cols / 2;
            const int yOffset = input.rows / 2;
            const size_t tailBytes = static_cast<size_t>(input.cols - xOffset) * sizeof(float);
            const size_t headBytes = static_cast<size_t>(xOffset) * sizeof(float);
            parallelForImageRows(input.cols, input.rows, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    const float* source = input.ptr<float>((y + yOffset) % input.rows);
                    float* destination = output.ptr<float>(y);
                    std::memcpy(destination, source + xOffset, tailBytes);
                    std::memcpy(destination + input.cols - xOffset, source, headBytes);
                }
            });
        }

        // The weight of the bandpass at the frequency (fx, fy) in cycles per pixel
        float maskValue(double fx,
                        double fy,
                        double minFeatureSize,
                        double maxFeatureSize,
                        FrequencyDomainFilterModule::FilterKind filterKind)
        {
            constexpr double kTwoPi = 2.0 * std::numbers::pi_v<double>;
            const double radiusSquared = (kTwoPi * fx) * (kTwoPi * fx) + (kTwoPi * fy) * (kTwoPi * fy);
            if (filterKind == FrequencyDomainFilterModule::FilterKind::Hard)
            {
                return radiusSquared * maxFeatureSize * maxFeatureSize > 1.0
                           && radiusSquared * minFeatureSize * minFeatureSize < 1.0
                       ? 1.0f
                       : 0.0f;
            }
            return static_cast<float>(std::exp(-radiusSquared * minFeatureSize * minFeatureSize / 2.0)
                                      - std::exp(-radiusSquared * maxFeatureSize * maxFeatureSize / 2.0));
        }

        // The frequency of DFT index k of n samples, in cycles per sample
        double frequency(int k, int n)
        {
            return static_cast<double>(k <= n / 2 ? k : k - n) / n;
        }

        // The mask in the layout of the complex DFT, packed like the real DFT of OpenCV (CCS), or
        // over the half spectrum of pocketfft. CCS keeps the real and imaginary parts of half the
        // spectrum in one real matrix: column 0, and the last column for an even width, hold the
        // columns kx = 0 and kx = width / 2 with ky in pairs of rows; the other columns hold
        // kx = (x + 1) / 2 for every row ky. The half spectrum holds kx = 0 .. width / 2.
        cv::Mat buildMask(const cv::Size& size,
                          double minFeatureSize,
                          double maxFeatureSize,
                          FrequencyDomainFilterModule::FilterKind filterKind,
                          FrequencyDomainFilterModule::MaskLayout layout)
        {
            using Layout = FrequencyDomainFilterModule::MaskLayout;
            const int columns = layout == Layout::Half ? size.width / 2 + 1 : size.width;
            cv::Mat mask(size.height, columns, CV_32F);
            parallelForImageRows(columns, size.height, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    float* row = mask.ptr<float>(y);
                    for (int x = 0; x < columns; ++x)
                    {
                        int kx = x;
                        int ky = y;
                        if (layout == Layout::Packed)
                        {
                            const bool edgeColumn = x == 0 || (size.width % 2 == 0 && x == size.width - 1);
                            kx = edgeColumn ? (x == 0 ? 0 : size.width / 2) : (x + 1) / 2;
                            ky = edgeColumn ? (y + 1) / 2 : y;
                        }
                        row[x] = maskValue(frequency(kx, size.width), frequency(ky, size.height),
                                           minFeatureSize, maxFeatureSize, filterKind);
                    }
                }
            });
            return mask;
        }

        cv::Mat spectrum(const cv::Mat* planes, cv::Mat& magnitude, cv::Mat& shifted)
        {
            cv::magnitude(planes[0], planes[1], magnitude);
            magnitude += cv::Scalar::all(1.0);
            cv::log(magnitude, magnitude);
            fftShift(magnitude, shifted);
            return shifted;
        }
    }

    std::unique_ptr<ProcessingModule> FrequencyDomainFilterModule::createRuntime() const
    {
        auto module = std::make_unique<FrequencyDomainFilterModule>();
        module->setParameters(parameters());
        return module;
    }

    const cv::Mat& FrequencyDomainFilterModule::maskForSize(const cv::Size& size, MaskLayout layout)
    {
        if (m_mask.empty()
            || m_maskSize != size
            || m_maskLayout != layout
            || m_maskMinFeatureSize != m_minFeatureSize
            || m_maskMaxFeatureSize != m_maxFeatureSize
            || m_maskFilterKind != m_filterKind)
        {
            m_mask = buildMask(size, m_minFeatureSize, m_maxFeatureSize, m_filterKind, layout);
            m_maskSize = size;
            m_maskLayout = layout;
            m_maskMinFeatureSize = m_minFeatureSize;
            m_maskMaxFeatureSize = m_maxFeatureSize;
            m_maskFilterKind = m_filterKind;
        }
        return m_mask;
    }

    void FrequencyDomainFilterModule::invalidateMask()
    {
        m_mask.release();
        m_maskSize = {};
    }

    ProcessingResult FrequencyDomainFilterModule::process(const ImageFrame& frame,
                                                          int processingBitDepth)
    {
        ImageFrame workingFrame;
        if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported frequency filter input"));
        }
        frameMatView(workingFrame).convertTo(m_grayFloat, CV_32F);

        try
        {
            if (m_outputMode == OutputMode::FilteredImage && m_fftLibrary == FftLibrary::Pocketfft)
            {
                return filterWithPocketfft(workingFrame);
            }
            const int rows = cv::getOptimalDFTSize(m_grayFloat.rows);
            const int columns = cv::getOptimalDFTSize(m_grayFloat.cols);
            cv::copyMakeBorder(m_grayFloat,
                               m_padded,
                               0,
                               rows - m_grayFloat.rows,
                               0,
                               columns - m_grayFloat.cols,
                               cv::BORDER_CONSTANT,
                               0);
            // The filtered image needs only the real DFT, packed half the size of the complex one,
            // and the real, even mask multiplies the packed parts in place
            if (m_outputMode == OutputMode::FilteredImage)
            {
                cv::dft(m_padded, m_packed);
                cv::multiply(m_packed, maskForSize(m_padded.size(), MaskLayout::Packed), m_packed);
                cv::dft(m_packed, m_filtered, cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);
                const cv::Mat visible = m_filtered(cv::Rect(0, 0, m_grayFloat.cols, m_grayFloat.rows));
                return ProcessingResult(outputFrame(visible, workingFrame));
            }

            cv::dft(m_padded, m_complex, cv::DFT_COMPLEX_OUTPUT);
            cv::split(m_complex, m_planes);
            if (m_outputMode == OutputMode::FilteredSpectrum)
            {
                const cv::Mat& mask = maskForSize(m_padded.size(), MaskLayout::Complex);
                cv::multiply(m_planes[0], mask, m_planes[0]);
                cv::multiply(m_planes[1], mask, m_planes[1]);
            }
            cv::Mat visible = spectrum(m_planes, m_spectrumMagnitude, m_shiftedSpectrum);
            if (visible.size() != m_grayFloat.size())
            {
                const int x = (visible.cols - m_grayFloat.cols) / 2;
                const int y = (visible.rows - m_grayFloat.rows) / 2;
                visible = visible(cv::Rect(x, y, m_grayFloat.cols, m_grayFloat.rows));
            }
            return ProcessingResult(outputFrame(visible, workingFrame));
        }
        catch (const std::exception& error)
        {
            return ProcessingResult(ImageFrame{},
                                    QString("Frequency domain filtering failed: %1")
                                        .arg(error.what()));
        }
    }

    // pocketfft transforms any size, so the frame is not padded, and runs on every core as
    // scipy.fft does with workers=-1. The real FFT gives the half spectrum kx = 0 .. width / 2.
    ProcessingResult FrequencyDomainFilterModule::filterWithPocketfft(const ImageFrame& workingFrame)
    {
        const int rows = m_grayFloat.rows;
        const int columns = m_grayFloat.cols;
        const int half = columns / 2 + 1;
        m_halfSpectrum.create(rows, half, CV_32FC2);
        m_filtered.create(rows, columns, CV_32F);
        auto* spectrum = reinterpret_cast<std::complex<float>*>(m_halfSpectrum.data);
        const pocketfft::shape_t shape{static_cast<size_t>(rows), static_cast<size_t>(columns)};
        const pocketfft::shape_t axes{0, 1};
        const pocketfft::stride_t imageStride{static_cast<ptrdiff_t>(m_grayFloat.step[0]),
                                              static_cast<ptrdiff_t>(sizeof(float))};
        const pocketfft::stride_t filteredStride{static_cast<ptrdiff_t>(m_filtered.step[0]),
                                                 static_cast<ptrdiff_t>(sizeof(float))};
        const pocketfft::stride_t spectrumStride{static_cast<ptrdiff_t>(m_halfSpectrum.step[0]),
                                                 static_cast<ptrdiff_t>(sizeof(std::complex<float>))};
        pocketfft::r2c(shape, imageStride, spectrumStride, axes, pocketfft::FORWARD,
                       m_grayFloat.ptr<float>(), spectrum, 1.0f, 0);
        const cv::Mat& mask = maskForSize(m_grayFloat.size(), MaskLayout::Half);
        parallelForImageRows(half, rows, [&](int firstRow, int lastRow)
        {
            for (int y = firstRow; y < lastRow; ++y)
            {
                const float* weights = mask.ptr<float>(y);
                auto* row = reinterpret_cast<std::complex<float>*>(m_halfSpectrum.ptr(y));
                for (int x = 0; x < half; ++x)
                {
                    row[x] *= weights[x];
                }
            }
        });
        // The inverse runs as two passes over the spectrum, as the 2D c2r of pocketfft allocates a
        // copy of the whole spectrum on every frame
        const pocketfft::shape_t spectrumShape{static_cast<size_t>(rows), static_cast<size_t>(half)};
        pocketfft::c2c(spectrumShape, spectrumStride, spectrumStride, {0}, pocketfft::BACKWARD,
                       spectrum, spectrum, 1.0f, 0);
        pocketfft::c2r(shape, spectrumStride, filteredStride, size_t{1}, pocketfft::BACKWARD,
                       spectrum, m_filtered.ptr<float>(), 1.0f / (static_cast<float>(rows) * columns), 0);
        return ProcessingResult(outputFrame(m_filtered, workingFrame));
    }

    QVariantMap FrequencyDomainFilterModule::parameters() const
    {
        return {{QStringLiteral("min_feature_size"), m_minFeatureSize},
                {QStringLiteral("max_feature_size"), m_maxFeatureSize},
                {QStringLiteral("filter_kind"), static_cast<int>(m_filterKind)},
                {QStringLiteral("output_mode"), static_cast<int>(m_outputMode)},
                {QStringLiteral("fft"), static_cast<int>(m_fftLibrary)}};
    }

    void FrequencyDomainFilterModule::setParameters(const QVariantMap& parameters)
    {
        const double oldMinimum = m_minFeatureSize;
        const double oldMaximum = m_maxFeatureSize;
        const FilterKind oldKind = m_filterKind;
        m_minFeatureSize = qMax(0.0,
                                parameters.value(QStringLiteral("min_feature_size"),
                                                 m_minFeatureSize).toDouble());
        m_maxFeatureSize = qMax(0.0,
                                parameters.value(QStringLiteral("max_feature_size"),
                                                 m_maxFeatureSize).toDouble());
        if (m_minFeatureSize > m_maxFeatureSize)
        {
            std::swap(m_minFeatureSize, m_maxFeatureSize);
        }
        m_filterKind = static_cast<FilterKind>(qBound(
            0,
            parameters.value(QStringLiteral("filter_kind"),
                             static_cast<int>(m_filterKind)).toInt(),
            1));
        m_outputMode = static_cast<OutputMode>(qBound(
            0,
            parameters.value(QStringLiteral("output_mode"),
                             static_cast<int>(m_outputMode)).toInt(),
            2));
        m_fftLibrary = static_cast<FftLibrary>(qBound(
            0,
            parameters.value(QStringLiteral("fft"),
                             static_cast<int>(m_fftLibrary)).toInt(),
            1));
        if (oldMinimum != m_minFeatureSize
            || oldMaximum != m_maxFeatureSize
            || oldKind != m_filterKind)
        {
            invalidateMask();
        }
    }
} // namespace scopeone::core::internal

namespace scopeone::core::internal
{
    SCOPEONE_REGISTER_PROCESSING_MODULE(
        FrequencyDomainFilterModule,
        QStringLiteral("frequency_domain_filter"),
        QStringLiteral("Frequency Domain Filter"),
        1,
        {choiceParameter("output_mode", "Output", 2,
                         {"Spectrum", "Filtered spectrum", "Filtered image"}),
         shownWhen(realParameter("min_feature_size", "Min feature size", 2.0,
                                 0.0, 1000.0, 0.1, 2),
                   "output_mode", {1, 2}),
         shownWhen(realParameter("max_feature_size", "Max feature size", 10.0,
                                 0.0, 1000.0, 0.1, 2),
                   "output_mode", {1, 2}),
         shownWhen(choiceParameter("filter_kind", "Filter kind", 0, {"Smooth", "Hard"}),
                   "output_mode", {1, 2}),
         shownWhen(choiceParameter("fft", "FFT", 1, {"OpenCV", "pocketfft"}),
                   "output_mode", {2})})
}
