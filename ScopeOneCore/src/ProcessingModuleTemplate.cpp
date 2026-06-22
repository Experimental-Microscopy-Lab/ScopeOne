#include "internal/ProcessingModuleTemplate.h"
#include "internal/FrameBufferUtils.h"

namespace scopeone::core::internal
{
    // Create the example processing module
    TemplateModule::TemplateModule(QObject* parent)
        : ProcessingModule(parent)
    {
    }

    // Forward frames after converting them to the requested processing format
    bool TemplateModule::process(const ModuleInput& in, ModuleOutput& out)
    {
        if (!in.frame.isValid())
        {
            out.frame = in.frame;
            out.error = "Invalid input";
            return false;
        }

        try
        {
            ImageFrame workingFrame;
            if (!convertFrameForProcessing(in.frame, workingFrame, in.processingBitDepth))
            {
                out.frame = in.frame;
                out.error = "Failed to convert frame to processing format";
                return false;
            }

            out.frame = workingFrame;
            return true;
        }
        catch (const std::exception& e)
        {
            out.frame = in.frame;
            out.error = QString("Template processing failed: %1").arg(e.what());
            return false;
        }
    }

    // Return the example module parameter map
    QVariantMap TemplateModule::getParameters() const
    {
        QVariantMap params;
        params["example_parameter"] = m_exampleParameter;
        return params;
    }

    // Apply the example parameter with a lower bound
    void TemplateModule::setParameters(const QVariantMap& params)
    {
        if (!params.contains("example_parameter"))
        {
            return;
        }

        int value = params.value("example_parameter").toInt();
        if (value < 1)
        {
            value = 1;
        }
        m_exampleParameter = value;
    }
} // namespace scopeone::core::internal
