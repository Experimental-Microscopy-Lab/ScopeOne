#include "scopeone/ProcessingPipeline.h"

#include "internal/ImageProcessingFramework.h"

namespace scopeone::core
{
    struct ProcessingPipeline::Impl
    {
        internal::ProcessingPipelineDefinition definition;
        std::shared_ptr<internal::ProcessingPipelineRuntime> runtime;

        void rebuildRuntime()
        {
            runtime = definition.createRuntime();
        }
    };

    ProcessingPipeline::ProcessingPipeline()
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->rebuildRuntime();
    }

    ProcessingPipeline::~ProcessingPipeline() = default;
    ProcessingPipeline::ProcessingPipeline(ProcessingPipeline&&) noexcept = default;
    ProcessingPipeline& ProcessingPipeline::operator=(ProcessingPipeline&&) noexcept = default;

    bool ProcessingPipeline::addModule(std::unique_ptr<ProcessingModule> module)
    {
        if (!module)
        {
            return false;
        }
        m_impl->definition.addModule(std::move(module));
        m_impl->rebuildRuntime();
        return true;
    }

    bool ProcessingPipeline::removeModule(int index)
    {
        const bool removed = m_impl->definition.removeModule(index);
        if (removed)
        {
            m_impl->rebuildRuntime();
        }
        return removed;
    }

    int ProcessingPipeline::moduleCount() const
    {
        return m_impl->definition.moduleCount();
    }

    ProcessingResult ProcessingPipeline::process(const ProcessingValue& input,
                                                 int processingBitDepth) const
    {
        return m_impl->runtime->processValue(input, processingBitDepth);
    }
}
