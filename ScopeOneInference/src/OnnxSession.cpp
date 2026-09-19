#include "OnnxInternal.h"

#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace
{
    scopeone::inference::TensorElementType tensorElementType(
        ONNXTensorElementDataType type)
    {
        using scopeone::inference::TensorElementType;
        switch (type)
        {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return TensorElementType::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return TensorElementType::UInt8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return TensorElementType::Int8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return TensorElementType::UInt16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: return TensorElementType::Int16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return TensorElementType::Int32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return TensorElementType::Int64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING: return TensorElementType::String;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return TensorElementType::Boolean;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return TensorElementType::Float16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return TensorElementType::Float64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32: return TensorElementType::UInt32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64: return TensorElementType::UInt64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64: return TensorElementType::Complex64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128: return TensorElementType::Complex128;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16: return TensorElementType::BFloat16;
        default: return TensorElementType::Unknown;
        }
    }

    std::size_t elementCount(const std::vector<std::int64_t>& shape)
    {
        return std::accumulate(shape.begin(), shape.end(), std::size_t{1},
                               [](std::size_t count, std::int64_t dimension)
                               {
                                   return count * static_cast<std::size_t>(dimension);
                               });
    }

    std::vector<scopeone::inference::TensorInfo> tensorMetadata(
        Ort::Session& session,
        bool input)
    {
        const std::size_t count = input ? session.GetInputCount() : session.GetOutputCount();
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<scopeone::inference::TensorInfo> metadata;
        metadata.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto name = input
                                  ? session.GetInputNameAllocated(index, allocator)
                                  : session.GetOutputNameAllocated(index, allocator);
            const auto typeInfo = input
                                      ? session.GetInputTypeInfo(index)
                                      : session.GetOutputTypeInfo(index);
            const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            metadata.push_back({name.get(),
                                tensorElementType(tensorInfo.GetElementType()),
                                tensorInfo.GetShape()});
        }
        return metadata;
    }
}

namespace scopeone::inference
{
    struct OnnxSession::Impl
    {
        std::shared_ptr<void> runtime;
        Ort::Session session{nullptr};
        std::vector<TensorInfo> inputMetadata;
        std::vector<TensorInfo> outputMetadata;
    };

    OnnxSession::OnnxSession(OnnxRuntime& runtime,
                             const std::filesystem::path& modelPath,
                             const SessionOptions& options)
        : m_impl(std::make_unique<Impl>())
    {
        const std::shared_ptr<OnnxRuntime::Impl> runtimeImpl = runtime.m_impl;
        m_impl->runtime = runtimeImpl;
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (options.provider == ExecutionProvider::Cuda)
        {
            if (!runtime.supports(ExecutionProvider::Cuda))
            {
                throw std::runtime_error("CUDAExecutionProvider is not available");
            }
            OrtCUDAProviderOptions cudaOptions{};
            cudaOptions.device_id = options.deviceId;
            sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
        }

        m_impl->session = Ort::Session(runtimeImpl->environment,
                                      modelPath.c_str(),
                                      sessionOptions);
        m_impl->inputMetadata = tensorMetadata(m_impl->session, true);
        m_impl->outputMetadata = tensorMetadata(m_impl->session, false);
    }

    OnnxSession::~OnnxSession() = default;
    OnnxSession::OnnxSession(OnnxSession&&) noexcept = default;
    OnnxSession& OnnxSession::operator=(OnnxSession&&) noexcept = default;

    const std::vector<TensorInfo>& OnnxSession::inputs() const
    {
        return m_impl->inputMetadata;
    }

    const std::vector<TensorInfo>& OnnxSession::outputs() const
    {
        return m_impl->outputMetadata;
    }

    std::vector<FloatTensor> OnnxSession::run(const std::vector<FloatTensor>& inputs)
    {
        if (inputs.size() != m_impl->inputMetadata.size())
        {
            throw std::runtime_error("Input tensor count does not match the model");
        }

        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputValues;
        std::vector<const char*> inputNames;
        inputValues.reserve(inputs.size());
        inputNames.reserve(inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index)
        {
            const FloatTensor& input = inputs[index];
            if (m_impl->inputMetadata[index].elementType != TensorElementType::Float32
                || elementCount(input.shape) != input.values.size())
            {
                throw std::runtime_error("Only valid float32 input tensors are supported");
            }
            inputValues.push_back(Ort::Value::CreateTensor<float>(
                memoryInfo,
                const_cast<float*>(input.values.data()),
                input.values.size(),
                input.shape.data(),
                input.shape.size()));
            inputNames.push_back(m_impl->inputMetadata[index].name.c_str());
        }

        std::vector<const char*> outputNames;
        outputNames.reserve(m_impl->outputMetadata.size());
        for (const TensorInfo& output : m_impl->outputMetadata)
        {
            outputNames.push_back(output.name.c_str());
        }

        std::vector<Ort::Value> outputValues = m_impl->session.Run(
            Ort::RunOptions{nullptr},
            inputNames.data(),
            inputValues.data(),
            inputValues.size(),
            outputNames.data(),
            outputNames.size());

        std::vector<FloatTensor> outputs;
        outputs.reserve(outputValues.size());
        for (Ort::Value& value : outputValues)
        {
            const auto info = value.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            {
                throw std::runtime_error("Only float32 output tensors are supported");
            }
            FloatTensor output;
            output.shape = info.GetShape();
            const std::size_t count = info.GetElementCount();
            const float* data = value.GetTensorData<float>();
            output.values.assign(data, data + count);
            outputs.push_back(std::move(output));
        }
        return outputs;
    }
}
