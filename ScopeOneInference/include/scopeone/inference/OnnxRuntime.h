#pragma once

#include "scopeone/inference/ExecutionProvider.h"
#include "scopeone/inference/InferenceExport.h"

#include <memory>
#include <string>
#include <vector>

namespace scopeone::inference
{
    class OnnxSession;

    class SCOPEONE_INFERENCE_EXPORT OnnxRuntime
    {
    public:
        OnnxRuntime();
        ~OnnxRuntime();

        OnnxRuntime(const OnnxRuntime&) = delete;
        OnnxRuntime& operator=(const OnnxRuntime&) = delete;
        OnnxRuntime(OnnxRuntime&&) noexcept;
        OnnxRuntime& operator=(OnnxRuntime&&) noexcept;

        std::string version() const;
        std::vector<std::string> availableProviders() const;
        bool supports(ExecutionProvider provider) const;

    private:
        struct Impl;
        std::shared_ptr<Impl> m_impl;

        friend class OnnxSession;
    };
}
