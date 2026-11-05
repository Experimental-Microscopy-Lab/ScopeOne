#pragma once

#include "internal/inference/ExecutionProvider.h"
#include "internal/inference/TensorInfo.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace scopeone::core::internal::inference
{
    class OnnxRuntime;

    struct SessionOptions
    {
        ExecutionProvider provider{ExecutionProvider::Cpu};
        int deviceId{0};
    };

    class OnnxSession
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
