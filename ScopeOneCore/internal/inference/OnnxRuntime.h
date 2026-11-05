#pragma once

#include "internal/inference/ExecutionProvider.h"

#include <memory>
#include <string>
#include <vector>

namespace scopeone::inference
{
    class OnnxSession;

    class OnnxRuntime
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
