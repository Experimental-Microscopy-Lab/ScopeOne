#include "OnnxInferenceModule.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace
{
    enum class TensorLayout
    {
        Nchw,
        Nhwc
    };

    std::vector<float> resizeImage(const std::vector<float>& source,
                                   int sourceWidth,
                                   int sourceHeight,
                                   int targetWidth,
                                   int targetHeight)
    {
        if (sourceWidth == targetWidth && sourceHeight == targetHeight)
        {
            return source;
        }

        std::vector<float> target(static_cast<std::size_t>(targetWidth) * targetHeight);
        const float scaleX = static_cast<float>(sourceWidth) / targetWidth;
        const float scaleY = static_cast<float>(sourceHeight) / targetHeight;
        for (int y = 0; y < targetHeight; ++y)
        {
            const float sourceY = std::clamp((y + 0.5f) * scaleY - 0.5f,
                                             0.0f,
                                             static_cast<float>(sourceHeight - 1));
            const int y0 = static_cast<int>(std::floor(sourceY));
            const int y1 = std::min(y0 + 1, sourceHeight - 1);
            const float fy = sourceY - y0;
            for (int x = 0; x < targetWidth; ++x)
            {
                const float sourceX = std::clamp((x + 0.5f) * scaleX - 0.5f,
                                                 0.0f,
                                                 static_cast<float>(sourceWidth - 1));
                const int x0 = static_cast<int>(std::floor(sourceX));
                const int x1 = std::min(x0 + 1, sourceWidth - 1);
                const float fx = sourceX - x0;
                const float top = source[static_cast<std::size_t>(y0) * sourceWidth + x0]
                    + fx * (source[static_cast<std::size_t>(y0) * sourceWidth + x1]
                            - source[static_cast<std::size_t>(y0) * sourceWidth + x0]);
                const float bottom = source[static_cast<std::size_t>(y1) * sourceWidth + x0]
                    + fx * (source[static_cast<std::size_t>(y1) * sourceWidth + x1]
                            - source[static_cast<std::size_t>(y1) * sourceWidth + x0]);
                target[static_cast<std::size_t>(y) * targetWidth + x] = top + fy * (bottom - top);
            }
        }
        return target;
    }

    TensorLayout resolveLayout(const scopeone::inference::TensorInfo& tensor,
                               int configuredLayout,
                               TensorLayout fallback)
    {
        if (configuredLayout == 1)
        {
            return TensorLayout::Nchw;
        }
        if (configuredLayout == 2)
        {
            return TensorLayout::Nhwc;
        }
        if (tensor.shape[1] == 1 && tensor.shape[3] != 1)
        {
            return TensorLayout::Nchw;
        }
        if (tensor.shape[3] == 1 && tensor.shape[1] != 1)
        {
            return TensorLayout::Nhwc;
        }
        return fallback;
    }

    int tensorHeight(const scopeone::inference::TensorInfo& tensor, TensorLayout layout)
    {
        return static_cast<int>(tensor.shape[layout == TensorLayout::Nchw ? 2 : 1]);
    }

    int tensorWidth(const scopeone::inference::TensorInfo& tensor, TensorLayout layout)
    {
        return static_cast<int>(tensor.shape[layout == TensorLayout::Nchw ? 3 : 2]);
    }
}

namespace scopeone::inference
{
    QString OnnxInferenceModule::id() const
    {
        return QStringLiteral("onnx_inference");
    }

    QString OnnxInferenceModule::name() const
    {
        return QStringLiteral("ONNX Inference");
    }

    QVariantMap OnnxInferenceModule::parameters() const
    {
        return {{QStringLiteral("model_path"), m_modelPath},
                {QStringLiteral("provider"), m_provider},
                {QStringLiteral("layout"), m_layout},
                {QStringLiteral("input_scale"), m_inputScale},
                {QStringLiteral("input_mean"), m_inputMean},
                {QStringLiteral("input_std"), m_inputStd},
                {QStringLiteral("output_mode"), m_outputMode}};
    }

    void OnnxInferenceModule::setParameters(const QVariantMap& parameters)
    {
        const QString modelPath = parameters.value(QStringLiteral("model_path"),
                                                   m_modelPath).toString();
        const int provider = parameters.value(QStringLiteral("provider"), m_provider).toInt();
        const int layout = parameters.value(QStringLiteral("layout"), m_layout).toInt();
        const double inputScale = parameters.value(QStringLiteral("input_scale"),
                                                   m_inputScale).toDouble();
        const double inputMean = parameters.value(QStringLiteral("input_mean"),
                                                  m_inputMean).toDouble();
        const double inputStd = parameters.value(QStringLiteral("input_std"),
                                                 m_inputStd).toDouble();
        const int outputMode = parameters.value(QStringLiteral("output_mode"),
                                                m_outputMode).toInt();
        if (modelPath == m_modelPath && provider == m_provider && layout == m_layout
            && inputScale == m_inputScale && inputMean == m_inputMean
            && inputStd == m_inputStd && outputMode == m_outputMode)
        {
            return;
        }
        m_modelPath = modelPath;
        m_provider = provider;
        m_layout = layout;
        m_inputScale = inputScale;
        m_inputMean = inputMean;
        m_inputStd = inputStd;
        m_outputMode = outputMode;
        m_session.reset();
        m_runtime.reset();
    }

    std::unique_ptr<core::ProcessingModule> OnnxInferenceModule::createRuntime() const
    {
        auto module = std::make_unique<OnnxInferenceModule>();
        module->setParameters(parameters());
        return module;
    }

    void OnnxInferenceModule::createSession()
    {
        auto runtime = std::make_unique<OnnxRuntime>();
        SessionOptions options;
        options.provider = m_provider == 0
                               ? ExecutionProvider::Cpu
                               : ExecutionProvider::Cuda;
#ifdef Q_OS_WIN
        const std::filesystem::path path(m_modelPath.toStdWString());
#else
        const std::filesystem::path path(m_modelPath.toStdString());
#endif
        auto session = std::make_unique<OnnxSession>(*runtime, path, options);

        const auto& inputs = session->inputs();
        const auto& outputs = session->outputs();
        if (inputs.size() != 1)
        {
            throw std::runtime_error("ONNX image model must have exactly one input");
        }
        if (outputs.size() != 1)
        {
            throw std::runtime_error("ONNX image model must have exactly one output");
        }
        if (inputs.front().elementType != TensorElementType::Float32)
        {
            throw std::runtime_error("ONNX image model input must be float32");
        }
        if (outputs.front().elementType != TensorElementType::Float32)
        {
            throw std::runtime_error("ONNX image model output must be float32");
        }
        if (inputs.front().shape.size() != 4)
        {
            throw std::runtime_error("ONNX image model input must have four dimensions");
        }
        if (outputs.front().shape.size() != 4)
        {
            throw std::runtime_error("ONNX image model output must have four dimensions");
        }
        m_runtime = std::move(runtime);
        m_session = std::move(session);
    }

    core::ProcessingResult OnnxInferenceModule::process(const core::ImageFrame& frame, int)
    {
        if (m_modelPath.isEmpty())
        {
            return {core::ImageFrame{}, QStringLiteral("Select an ONNX model")};
        }

        try
        {
            if (!m_session)
            {
                createSession();
            }

            const auto& inputInfo = m_session->inputs().front();
            const TensorLayout inputLayout = resolveLayout(inputInfo,
                                                           m_layout,
                                                           TensorLayout::Nchw);
            const int channelIndex = inputLayout == TensorLayout::Nchw ? 1 : 3;
            if (inputInfo.shape[0] > 0 && inputInfo.shape[0] != 1)
            {
                throw std::runtime_error("ONNX image model batch size must be one or dynamic");
            }
            if (inputInfo.shape[channelIndex] > 0 && inputInfo.shape[channelIndex] != 1)
            {
                throw std::runtime_error("ONNX image model input must be single-channel");
            }

            std::vector<float> source(static_cast<std::size_t>(frame.width) * frame.height);
            const float frameScale = 1.0f / static_cast<float>(frame.maxValue());
            for (int y = 0; y < frame.height; ++y)
            {
                float* destination = source.data() + static_cast<std::size_t>(y) * frame.width;
                if (frame.isMono16())
                {
                    const auto* source = reinterpret_cast<const quint16*>(
                        frame.bytes.constData() + static_cast<qsizetype>(y) * frame.stride);
                    for (int x = 0; x < frame.width; ++x)
                    {
                        destination[x] = static_cast<float>(source[x]) * frameScale;
                    }
                }
                else
                {
                    const auto* source = reinterpret_cast<const quint8*>(
                        frame.bytes.constData() + static_cast<qsizetype>(y) * frame.stride);
                    for (int x = 0; x < frame.width; ++x)
                    {
                        destination[x] = static_cast<float>(source[x]) * frameScale;
                    }
                }
            }

            const int modelHeight = tensorHeight(inputInfo, inputLayout) > 0
                                        ? tensorHeight(inputInfo, inputLayout)
                                        : frame.height;
            const int modelWidth = tensorWidth(inputInfo, inputLayout) > 0
                                       ? tensorWidth(inputInfo, inputLayout)
                                       : frame.width;
            std::vector<float> modelInput = resizeImage(source,
                                                        frame.width,
                                                        frame.height,
                                                        modelWidth,
                                                        modelHeight);
            for (float& value : modelInput)
            {
                value = static_cast<float>((value * m_inputScale - m_inputMean) / m_inputStd);
            }

            FloatTensor input;
            input.shape = inputLayout == TensorLayout::Nchw
                              ? std::vector<std::int64_t>{1, 1, modelHeight, modelWidth}
                              : std::vector<std::int64_t>{1, modelHeight, modelWidth, 1};
            input.values = modelInput;
            std::vector<FloatTensor> outputs = m_session->run({std::move(input)});
            FloatTensor& output = outputs.front();
            const TensorLayout outputLayout = resolveLayout(m_session->outputs().front(),
                                                            m_layout,
                                                            inputLayout);
            const int outputChannelIndex = outputLayout == TensorLayout::Nchw ? 1 : 3;
            if (output.shape[0] != 1 || output.shape[outputChannelIndex] != 1)
            {
                throw std::runtime_error("ONNX image model output must be single-channel");
            }

            const int outputHeight = static_cast<int>(
                output.shape[outputLayout == TensorLayout::Nchw ? 2 : 1]);
            const int outputWidth = static_cast<int>(
                output.shape[outputLayout == TensorLayout::Nchw ? 3 : 2]);
            if (output.values.size()
                != static_cast<std::size_t>(outputWidth) * outputHeight)
            {
                throw std::runtime_error("ONNX output tensor size is invalid");
            }

            if (m_outputMode == 1)
            {
                if (output.values.size() != modelInput.size())
                {
                    throw std::runtime_error("Residual output size must match the model input");
                }
                for (std::size_t index = 0; index < output.values.size(); ++index)
                {
                    output.values[index] = modelInput[index] - output.values[index];
                }
            }
            for (float& value : output.values)
            {
                value = static_cast<float>((value * m_inputStd + m_inputMean) / m_inputScale);
            }

            std::vector<float> displayOutput = resizeImage(output.values,
                                                           outputWidth,
                                                           outputHeight,
                                                           frame.width,
                                                           frame.height);
            QByteArray bytes(static_cast<qsizetype>(displayOutput.size() * sizeof(quint16)),
                             Qt::Uninitialized);
            auto* destination = reinterpret_cast<quint16*>(bytes.data());
            for (std::size_t index = 0; index < displayOutput.size(); ++index)
            {
                destination[index] = static_cast<quint16>(std::lround(
                    std::clamp(displayOutput[index], 0.0f, 1.0f) * 65535.0f));
            }

            core::ImageFrame result = frame;
            result.stride = frame.width * static_cast<int>(sizeof(quint16));
            result.bitsPerSample = 16;
            result.pixelFormat = core::ImagePixelFormat::Mono16;
            result.bytes = std::move(bytes);
            return {std::move(result), {}};
        }
        catch (const std::exception& error)
        {
            return {core::ImageFrame{}, QString::fromUtf8(error.what())};
        }
    }
}
