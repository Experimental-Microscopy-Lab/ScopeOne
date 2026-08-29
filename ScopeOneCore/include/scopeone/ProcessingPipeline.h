#pragma once

#include "scopeone/ProcessingPlugin.h"
#include "scopeone/scopeone_core_export.h"

#include <memory>

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT ProcessingPipeline
    {
    public:
        ProcessingPipeline();
        ~ProcessingPipeline();

        ProcessingPipeline(ProcessingPipeline&&) noexcept;
        ProcessingPipeline& operator=(ProcessingPipeline&&) noexcept;

        ProcessingPipeline(const ProcessingPipeline&) = delete;
        ProcessingPipeline& operator=(const ProcessingPipeline&) = delete;

        bool addModule(std::unique_ptr<ProcessingModule> module);
        bool removeModule(int index);
        bool setModuleParameters(int index, const QVariantMap& parameters);
        bool resetModuleState(int index);
        int moduleCount() const;

        ProcessingResult process(const ProcessingValue& input, int processingBitDepth = 16) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
