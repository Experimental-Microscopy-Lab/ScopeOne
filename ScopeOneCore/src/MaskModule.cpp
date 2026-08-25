#include "internal/MaskModule.h"

#include "internal/FrameBufferUtils.h"

#include <cmath>
#include <numbers>
#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    namespace
    {
        float smoothBoundary(float distance, double edgeWidth)
        {
            if (edgeWidth <= 0.0)
            {
                return distance <= 0.0f ? 1.0f : 0.0f;
            }
            const double value = qBound(0.0, 0.5 - static_cast<double>(distance) / edgeWidth, 1.0);
            return static_cast<float>(value * value * (3.0 - 2.0 * value));
        }

        cv::Mat buildMask(const cv::Size& size,
                          int shape,
                          double centerX,
                          double centerY,
                          double sizeX,
                          double sizeY,
                          double innerSize,
                          double rotation,
                          double edgeWidth,
                          bool invert)
        {
            cv::Mat mask(size, CV_32F);
            const double angle = rotation * std::numbers::pi_v<double> / 180.0;
            const double cosine = std::cos(angle);
            const double sine = std::sin(angle);
            const double halfWidth = qMax(sizeX, 1.0e-9) / 2.0;
            const double halfHeight = qMax(sizeY, 1.0e-9) / 2.0;
            const double innerRadius = qBound(0.0, innerSize, 1.0);
            const double outerRadius = qMax(halfWidth, 1.0e-9);

            for (int y = 0; y < size.height; ++y)
            {
                float* row = mask.ptr<float>(y);
                const double frequencyY = (static_cast<double>(y) - size.height / 2.0)
                    / static_cast<double>(size.height);
                for (int x = 0; x < size.width; ++x)
                {
                    const double frequencyX = (static_cast<double>(x) - size.width / 2.0)
                        / static_cast<double>(size.width);
                    const double dx = frequencyX - centerX;
                    const double dy = frequencyY - centerY;
                    const double rotatedX = cosine * dx + sine * dy;
                    const double rotatedY = -sine * dx + cosine * dy;
                    float value = 0.0f;

                    if (shape == 1)
                    {
                        const double distance = qMax(std::abs(rotatedX) - halfWidth,
                                                     std::abs(rotatedY) - halfHeight);
                        value = smoothBoundary(static_cast<float>(distance), edgeWidth);
                    }
                    else if (shape == 2)
                    {
                        const double radius = std::sqrt(rotatedX * rotatedX + rotatedY * rotatedY);
                        const double outerDistance = radius - outerRadius;
                        const double innerDistance = innerRadius - radius;
                        value = qMin(smoothBoundary(static_cast<float>(outerDistance), edgeWidth),
                                     smoothBoundary(static_cast<float>(innerDistance), edgeWidth));
                    }
                    else
                    {
                        const double distance = std::sqrt(
                            (rotatedX / halfWidth) * (rotatedX / halfWidth)
                            + (rotatedY / halfHeight) * (rotatedY / halfHeight)) - 1.0;
                        value = smoothBoundary(static_cast<float>(distance), edgeWidth);
                    }
                    row[x] = invert ? 1.0f - value : value;
                }
            }
            return mask;
        }

        ImageFrame maskPreview(const ComplexFrame& source, const cv::Mat& mask)
        {
            cv::Mat preview;
            mask.convertTo(preview, CV_8U, 255.0);
            return makeMono8Frame(source.sourceId, preview.cols, preview.rows, copyMatBytes(preview));
        }
    }

    std::unique_ptr<ProcessingModule> MaskModule::createRuntime() const
    {
        auto module = std::make_unique<MaskModule>();
        module->setParameters(parameters());
        return module;
    }

    const cv::Mat& MaskModule::maskForSize(const cv::Size& size)
    {
        const QVariantMap currentParameters = parameters();
        if (m_mask.empty() || m_maskSize != size || m_maskParameters != currentParameters)
        {
            m_mask = buildMask(size,
                               m_shape,
                               m_centerX,
                               m_centerY,
                               m_sizeX,
                               m_sizeY,
                               m_innerSize,
                               m_rotation,
                               m_edgeWidth,
                               m_invert);
            m_maskSize = size;
            m_maskParameters = currentParameters;
        }
        return m_mask;
    }

    const cv::Mat& MaskModule::frequencyMaskForSize(const cv::Size& size)
    {
        const cv::Mat& centeredMask = maskForSize(size);
        if (m_frequencyMask.empty() || m_frequencyMask.size() != size)
        {
            m_frequencyMask.create(size, CV_32F);
            const int xOffset = size.width / 2;
            const int yOffset = size.height / 2;
            for (int y = 0; y < size.height; ++y)
            {
                const float* source = centeredMask.ptr<float>((y + yOffset) % size.height);
                float* output = m_frequencyMask.ptr<float>(y);
                for (int x = 0; x < size.width; ++x)
                {
                    output[x] = source[(x + xOffset) % size.width];
                }
            }
        }
        return m_frequencyMask;
    }

    void MaskModule::invalidateMask()
    {
        m_mask.release();
        m_frequencyMask.release();
        m_maskSize = {};
        m_maskParameters.clear();
    }

    ProcessingResult MaskModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid mask input"));
        }

        ImageFrame workingFrame;
        if (!convertFrameForProcessing(frame, workingFrame, processingBitDepth))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported mask input"));
        }

        const cv::Mat& mask = maskForSize({workingFrame.width, workingFrame.height});
        QByteArray bytes = workingFrame.isMono16()
                               ? allocatePixelBytes<quint16>(workingFrame.width, workingFrame.height)
                               : allocatePixelBytes<uchar>(workingFrame.width, workingFrame.height);
        if (bytes.isEmpty())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Failed to allocate mask output"));
        }

        if (workingFrame.isMono16())
        {
            parallelForImageRows(workingFrame.width, workingFrame.height, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    const quint16* source = frameRowData<quint16>(workingFrame, y);
                    const float* weights = mask.ptr<float>(y);
                    quint16* output = reinterpret_cast<quint16*>(
                        bytes.data() + static_cast<qint64>(y) * workingFrame.width * sizeof(quint16));
                    for (int x = 0; x < workingFrame.width; ++x)
                    {
                        output[x] = static_cast<quint16>(source[x] * weights[x]);
                    }
                }
            });
        }
        else
        {
            parallelForImageRows(workingFrame.width, workingFrame.height, [&](int firstRow, int lastRow)
            {
                for (int y = firstRow; y < lastRow; ++y)
                {
                    const uchar* source = frameRowData<uchar>(workingFrame, y);
                    const float* weights = mask.ptr<float>(y);
                    uchar* output = reinterpret_cast<uchar*>(
                        bytes.data() + static_cast<qint64>(y) * workingFrame.width);
                    for (int x = 0; x < workingFrame.width; ++x)
                    {
                        output[x] = static_cast<uchar>(source[x] * weights[x]);
                    }
                }
            });
        }
        return ProcessingResult(makeFrameLike(workingFrame,
                                              workingFrame.width,
                                              workingFrame.height,
                                              std::move(bytes)));
    }

    ProcessingResult MaskModule::processValue(const ProcessingValue& input, int processingBitDepth)
    {
        if (std::holds_alternative<ImageFrame>(input))
        {
            return process(std::get<ImageFrame>(input), processingBitDepth);
        }
        if (!std::holds_alternative<ComplexFrame>(input))
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported mask input"));
        }
        const ComplexFrame& source = std::get<ComplexFrame>(input);
        if (!source.isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid complex field"));
        }

        try
        {
            const cv::Size size(source.width, source.height);
            const cv::Mat& previewMask = maskForSize(size);
            const cv::Mat& mask = frequencyMaskForSize(size);
            const cv::Mat real(source.height, source.width, CV_32F,
                               const_cast<char*>(source.real.constData()),
                               source.stride * static_cast<int>(sizeof(float)));
            const cv::Mat imaginary(source.height, source.width, CV_32F,
                                    const_cast<char*>(source.imaginary.constData()),
                                    source.stride * static_cast<int>(sizeof(float)));
            cv::Mat maskedReal;
            cv::Mat maskedImaginary;
            cv::multiply(real, mask, maskedReal);
            cv::multiply(imaginary, mask, maskedImaginary);

            ComplexFrame output = source;
            output.real = copyMatBytes(maskedReal);
            output.imaginary = copyMatBytes(maskedImaginary);
            output.stride = source.width;
            ProcessingResult result(ProcessingValue{std::move(output)});
            result.frame = maskPreview(source, previewMask);
            return result;
        }
        catch (const std::exception& e)
        {
            return ProcessingResult(ImageFrame{}, QString("Mask processing failed: %1").arg(e.what()));
        }
    }

    QVariantMap MaskModule::parameters() const
    {
        return {{QStringLiteral("shape"), m_shape},
                {QStringLiteral("center_x"), m_centerX},
                {QStringLiteral("center_y"), m_centerY},
                {QStringLiteral("size_x"), m_sizeX},
                {QStringLiteral("size_y"), m_sizeY},
                {QStringLiteral("inner_size"), m_innerSize},
                {QStringLiteral("rotation"), m_rotation},
                {QStringLiteral("edge_width"), m_edgeWidth},
                {QStringLiteral("invert"), m_invert}};
    }

    void MaskModule::setParameters(const QVariantMap& parameters)
    {
        const QVariantMap oldParameters = this->parameters();
        m_shape = qBound(0, parameters.value(QStringLiteral("shape"), m_shape).toInt(), 2);
        m_centerX = qBound(-0.5, parameters.value(QStringLiteral("center_x"), m_centerX).toDouble(), 0.5);
        m_centerY = qBound(-0.5, parameters.value(QStringLiteral("center_y"), m_centerY).toDouble(), 0.5);
        m_sizeX = qBound(0.001, parameters.value(QStringLiteral("size_x"), m_sizeX).toDouble(), 1.0);
        m_sizeY = qBound(0.001, parameters.value(QStringLiteral("size_y"), m_sizeY).toDouble(), 1.0);
        m_innerSize = qBound(0.0, parameters.value(QStringLiteral("inner_size"), m_innerSize).toDouble(), 1.0);
        m_rotation = qBound(-180.0, parameters.value(QStringLiteral("rotation"), m_rotation).toDouble(), 180.0);
        m_edgeWidth = qBound(0.0, parameters.value(QStringLiteral("edge_width"), m_edgeWidth).toDouble(), 0.5);
        m_invert = parameters.value(QStringLiteral("invert"), m_invert).toBool();
        if (oldParameters != this->parameters())
        {
            invalidateMask();
        }
    }
}
