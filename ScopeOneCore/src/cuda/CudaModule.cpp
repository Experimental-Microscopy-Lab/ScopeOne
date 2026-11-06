#include "internal/cuda/CudaModule.h"

#include "internal/cuda/CudaDevice.h"

#include <utility>

namespace scopeone::core::internal::cuda
{
    CudaModule::CudaModule(GpuMemoryLayout layout)
        : m_layout(layout)
    {
    }

    CudaModule::~CudaModule() = default;

    scopeone::core::ProcessingResult CudaModule::process(
        const scopeone::core::ImageFrame& frame,
        int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return {scopeone::core::ImageFrame{}, QStringLiteral("Invalid input frame")};
        }
        if (!isCudaDeviceAvailable())
        {
            return {scopeone::core::ImageFrame{}, QStringLiteral("CUDA device unavailable")};
        }
        const GpuImageSize size = outputSize(frame.width, frame.height);
        if (size.width <= 0 || size.height <= 0)
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("Frame too small for this module")};
        }
        if ((!m_inputBuffer.isValid()
             || m_inputBuffer.width() != frame.width
             || m_inputBuffer.height() != frame.height
             || m_inputBuffer.layout() != m_layout)
            && !m_inputBuffer.allocate(frame.width, frame.height, m_layout))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA frame allocation failed")};
        }
        if ((!m_outputBuffer.isValid()
             || m_outputBuffer.width() != size.width
             || m_outputBuffer.height() != size.height
             || m_outputBuffer.layout() != m_layout)
            && !m_outputBuffer.allocate(size.width, size.height, m_layout))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA frame allocation failed")};
        }
        if (!m_inputBuffer.upload(frame))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA input upload failed")};
        }
        if (!processDevice(m_inputBuffer, m_outputBuffer, processingBitDepth))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA processing failed")};
        }

        scopeone::core::ImageFrame output;
        output.cameraId = frame.cameraId;
        output.width = size.width;
        output.height = size.height;
        output.pixelFormat = processingBitDepth >= 16
            ? scopeone::core::ImagePixelFormat::Mono16
            : scopeone::core::ImagePixelFormat::Mono8;
        output.bitsPerSample = processingBitDepth >= 16 ? 16 : 8;
        output.stride = output.width * output.bytesPerPixel();
        output.frameIndex = frame.frameIndex;
        output.timestampNs = frame.timestampNs;
        output.sourceRoiX = frame.sourceRoiX;
        output.sourceRoiY = frame.sourceRoiY;
        output.sourceRoiWidth = frame.sourceRoiWidth;
        output.sourceRoiHeight = frame.sourceRoiHeight;
        output.bytes.resize(static_cast<int>(output.payloadByteCount()));
        const GpuOutputMapping mapping = outputMapping(processingBitDepth);
        if (!m_outputBuffer.download(output, mapping.scale, mapping.offset))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA output download failed")};
        }
        return {std::move(output), {}};
    }
}
