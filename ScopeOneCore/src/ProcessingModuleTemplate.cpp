#include "internal/ProcessingModuleTemplate.h"
#include "internal/FrameBufferUtils.h"

namespace scopeone::core::internal
{
    // Creates a pass through processing module template
    TemplateModule::TemplateModule(QObject* parent)
        : ProcessingModule(parent)
    {
    }

    // Converts the input to the selected processing bit depth
    bool TemplateModule::process(const ModuleInput& in, ModuleOutput& out)
    {
        if (!in.frame.isValid())
        {
            out.frame = in.frame;
            out.error = "Invalid input";
            return false;
        }

        ImageFrame workingFrame;
        if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth))
        {
            out.frame = in.frame;
            out.error = "Unsupported input frame";
            return false;
        }

        out.frame = workingFrame;
        return true;
    }

    // Returns the template parameter map
    QVariantMap TemplateModule::getParameters() const
    {
        return {};
    }

    // Accepts template parameters for future derived examples
    void TemplateModule::setParameters(const QVariantMap&)
    {
    }
} // namespace scopeone::core::internal
