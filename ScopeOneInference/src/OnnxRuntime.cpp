#include "OnnxInternal.h"

#include <algorithm>
#include <utility>

namespace scopeone::inference
{
    OnnxRuntime::Impl::Impl()
        : environment(ORT_LOGGING_LEVEL_WARNING, "ScopeOneInference")
    {
    }

    OnnxRuntime::OnnxRuntime()
        : m_impl(std::make_shared<Impl>())
    {
    }

    OnnxRuntime::~OnnxRuntime() = default;
    OnnxRuntime::OnnxRuntime(OnnxRuntime&&) noexcept = default;
    OnnxRuntime& OnnxRuntime::operator=(OnnxRuntime&&) noexcept = default;

    std::string OnnxRuntime::version() const
    {
        return Ort::GetVersionString();
    }

    std::vector<std::string> OnnxRuntime::availableProviders() const
    {
        return Ort::GetAvailableProviders();
    }

    bool OnnxRuntime::supports(ExecutionProvider provider) const
    {
        if (provider == ExecutionProvider::Cpu)
        {
            return true;
        }
        const auto providers = availableProviders();
        return std::find(providers.begin(), providers.end(), "CUDAExecutionProvider")
               != providers.end();
    }
}
