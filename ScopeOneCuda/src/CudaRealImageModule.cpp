#include "scopeone/cuda/CudaRealImageModule.h"

#include "scopeone/cuda/CudaDevice.h"

#include <utility>

namespace scopeone::cuda
{
    CudaRealImageModule::CudaRealImageModule(GpuMemoryLayout layout)
        : m_layout(layout)
    {
    }

    CudaRealImageModule::~CudaRealImageModule() = default;

    scopeone::core::ProcessingResult CudaRealImageModule::process(
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
        if (!m_inputBuffer.isValid()
            || m_inputBuffer.width() != frame.width
            || m_inputBuffer.height() != frame.height
            || m_inputBuffer.layout() != m_layout)
        {
            if (!m_inputBuffer.allocate(frame.width, frame.height, m_layout)
                || !m_outputBuffer.allocate(frame.width, frame.height, m_layout))
            {
                return {scopeone::core::ImageFrame{},
                        QStringLiteral("CUDA frame allocation failed")};
            }
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
        output.width = frame.width;
        output.height = frame.height;
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
        if (!m_outputBuffer.download(output))
        {
            return {scopeone::core::ImageFrame{},
                    QStringLiteral("CUDA output download failed")};
        }
        return {std::move(output), {}};
    }
}
