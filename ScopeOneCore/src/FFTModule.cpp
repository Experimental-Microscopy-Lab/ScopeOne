#include "internal/FFTModule.h"

#include "internal/FrameBufferUtils.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal
{
    namespace
    {
        bool frameToFloat(const ImageFrame& frame, cv::Mat& output)
        {
            if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16()))
            {
                return false;
            }
            const int type = frame.isMono16() ? CV_16UC1 : CV_8UC1;
            const cv::Mat input(frame.height,
                                frame.width,
                                type,
                                const_cast<char*>(frame.bytes.constData()),
                                frame.stride);
            input.convertTo(output, CV_32F);
            return true;
        }

        void fftShift(const cv::Mat& input, cv::Mat& output)
        {
            output.create(input.size(), input.type());
            const int xOffset = (input.cols + 1) / 2;
            const int yOffset = (input.rows + 1) / 2;
            parallelForImageRows(input.cols, input.rows, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    const float* source = input.ptr<float>((y + yOffset) % input.rows);
                    float* destination = output.ptr<float>(y);
                    for (int x = 0; x < input.cols; ++x)
                    {
                        destination[x] = source[(x + xOffset) % input.cols];
                    }
                }
            });
        }

        ImageFrame spectrumFrame(const cv::Mat* planes,
                                 cv::Mat& magnitude,
                                 cv::Mat& shifted,
                                 const ImageFrame& reference)
        {
            cv::magnitude(planes[0], planes[1], magnitude);
            magnitude += cv::Scalar::all(1.0);
            cv::log(magnitude, magnitude);
            fftShift(magnitude, shifted);

            cv::Mat visible = shifted;
            if (visible.cols != reference.width || visible.rows != reference.height)
            {
                const int x = (visible.cols - reference.width) / 2;
                const int y = (visible.rows - reference.height) / 2;
                visible = visible(cv::Rect(x, y, reference.width, reference.height));
            }
            cv::Mat preview;
            cv::normalize(visible, preview, 0.0, 255.0, cv::NORM_MINMAX, CV_8U);
            ImageFrame frame = makeMono8Frame(reference.cameraId,
                                              preview.cols,
                                              preview.rows,
                                              copyMatBytes(preview));
            copyFrameMetadata(reference, frame);
            return frame;
        }
    }

    std::unique_ptr<ProcessingModule> FFTModule::createRuntime() const
    {
        return std::make_unique<FFTModule>();
    }

    ProcessingResult FFTModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        return processValue(ProcessingValue{frame}, processingBitDepth);
    }

    ProcessingResult FFTModule::processValue(const ProcessingValue& input,
                                             int processingBitDepth)
    {
        if (!std::holds_alternative<ImageFrame>(input))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("FFT requires an image"));
        }

        const ImageFrame& frame = std::get<ImageFrame>(input);
        ImageFrame workingFrame;
        if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth)
            || !frameToFloat(workingFrame, m_grayFloat))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported FFT input"));
        }

        try
        {
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
            cv::dft(m_padded, m_complex, cv::DFT_COMPLEX_OUTPUT);
            cv::split(m_complex, m_planes);

            ComplexFrame output;
            output.sourceId = frame.cameraId;
            output.width = columns;
            output.height = rows;
            output.stride = columns;
            output.sourceWidth = frame.width;
            output.sourceHeight = frame.height;
            output.frameIndex = frame.frameIndex;
            output.timestampNs = frame.timestampNs;
            output.real = copyMatBytes(m_planes[0]);
            output.imaginary = copyMatBytes(m_planes[1]);

            ProcessingResult result(ProcessingValue{std::move(output)});
            result.frame = spectrumFrame(m_planes,
                                         m_spectrumMagnitude,
                                         m_shiftedSpectrum,
                                         workingFrame);
            return result;
        }
        catch (const std::exception& error)
        {
            return ProcessingResult(ImageFrame{},
                                    QString("FFT failed: %1").arg(error.what()));
        }
    }

    QVariantMap FFTModule::parameters() const
    {
        return {};
    }

    void FFTModule::setParameters(const QVariantMap&)
    {
    }
} // namespace scopeone::core::internal
