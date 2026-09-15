#include "internal/IFFTModule.h"

#include "internal/FrameBufferUtils.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <cstring>

namespace scopeone::core::internal
{
    std::unique_ptr<ProcessingModule> IFFTModule::createRuntime() const
    {
        return std::make_unique<IFFTModule>();
    }

    ProcessingResult IFFTModule::process(const ImageFrame&, int)
    {
        return ProcessingResult(ImageFrame{}, QStringLiteral("IFFT requires a complex field"));
    }

    ProcessingResult IFFTModule::processValue(const ProcessingValue& input, int processingBitDepth)
    {
        if (!std::holds_alternative<ComplexFrame>(input))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("IFFT requires a complex field"));
        }
        const ComplexFrame& source = std::get<ComplexFrame>(input);
        if (!source.isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid complex field"));
        }

        try
        {
            cv::Mat real(source.height, source.width, CV_32F,
                         const_cast<char*>(source.real.constData()),
                         source.stride * static_cast<int>(sizeof(float)));
            cv::Mat imaginary(source.height, source.width, CV_32F,
                              const_cast<char*>(source.imaginary.constData()),
                              source.stride * static_cast<int>(sizeof(float)));
            cv::Mat planes[] = {real, imaginary};
            cv::Mat complex;
            cv::merge(planes, 2, complex);
            cv::Mat inverse;
            cv::dft(complex, inverse, cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);

            const cv::Mat visible = source.sourceWidth > 0
                                         && source.sourceHeight > 0
                                         && source.sourceWidth <= inverse.cols
                                         && source.sourceHeight <= inverse.rows
                                     ? inverse(cv::Rect(0,
                                                        0,
                                                        source.sourceWidth,
                                                        source.sourceHeight))
                                     : inverse;

            const int bits = processingBitDepth >= 16 ? 16 : 8;
            const int type = bits == 16 ? CV_16U : CV_8U;
            const int maxValue = bits == 16 ? 65535 : 255;
            cv::Mat output(visible.size(), type);
            cv::normalize(visible, output, 0.0, maxValue, cv::NORM_MINMAX, type);
            QByteArray bytes(static_cast<qsizetype>(output.total() * output.elemSize()), Qt::Uninitialized);
            std::memcpy(bytes.data(), output.data, static_cast<size_t>(bytes.size()));
            ImageFrame frame;
            frame.cameraId = source.sourceId;
            frame.width = output.cols;
            frame.height = output.rows;
            frame.stride = static_cast<int>(output.step);
            frame.bitsPerSample = bits;
            frame.pixelFormat = bits == 16 ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
            frame.frameIndex = source.frameIndex;
            frame.timestampNs = source.timestampNs;
            frame.bytes = std::move(bytes);
            return ProcessingResult(std::move(frame));
        }
        catch (const std::exception& e)
        {
            return ProcessingResult(ImageFrame{}, QString("IFFT failed: %1").arg(e.what()));
        }
    }
}
