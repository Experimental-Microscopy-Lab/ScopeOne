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

    bool ProcessingPipeline::setModuleParameters(int index, const QVariantMap& parameters)
    {
        const bool updated = m_impl->definition.withModule(index, [&parameters](ProcessingModule* module)
        {
            module->setParameters(parameters);
        });
        if (updated)
        {
            m_impl->rebuildRuntime();
        }
        return updated;
    }

    bool ProcessingPipeline::resetModuleState(int index)
    {
        bool reset = false;
        m_impl->definition.withModule(index, [&reset](ProcessingModule* module)
        {
            reset = module->resetState();
        });
        if (reset)
        {
            m_impl->rebuildRuntime();
        }
        return reset;
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
