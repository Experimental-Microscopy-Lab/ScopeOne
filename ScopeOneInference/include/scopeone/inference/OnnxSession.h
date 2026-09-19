#pragma once

#include "scopeone/inference/ExecutionProvider.h"
#include "scopeone/inference/InferenceExport.h"
#include "scopeone/inference/TensorInfo.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace scopeone::inference
{
    class OnnxRuntime;

    struct SessionOptions
    {
        ExecutionProvider provider{ExecutionProvider::Cpu};
        int deviceId{0};
    };

    class SCOPEONE_INFERENCE_EXPORT OnnxSession
    {
    public:
        OnnxSession(OnnxRuntime& runtime,
                    const std::filesystem::path& modelPath,
                    const SessionOptions& options = {});
        ~OnnxSession();

        OnnxSession(const OnnxSession&) = delete;
        OnnxSession& operator=(const OnnxSession&) = delete;
        OnnxSession(OnnxSession&&) noexcept;
        OnnxSession& operator=(OnnxSession&&) noexcept;

        const std::vector<TensorInfo>& inputs() const;
        const std::vector<TensorInfo>& outputs() const;
        std::vector<FloatTensor> run(const std::vector<FloatTensor>& inputs);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
