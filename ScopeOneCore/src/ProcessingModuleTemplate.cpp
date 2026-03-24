#include "internal/ProcessingModuleTemplate.h"
#include "internal/FrameBufferUtils.h"

namespace scopeone::core::internal {

TemplateModule::TemplateModule(QObject* parent)
    : ProcessingModule(parent)
{
}

bool TemplateModule::process(const ModuleInput& in, ModuleOutput& out)
{
    // Minimal example module that forwards mono8 data
    if (!in.frame.isValid()) {
        out.frame = in.frame;
        out.error = "Invalid input";
        return false;
    }

    try {
        ImageFrame gray;
        if (!convertFrameToMono8(in.frame, gray)) {
            out.frame = in.frame;
            out.error = "Failed to convert frame to grayscale";
            return false;
        }

        out.frame = gray;
        return true;
    } catch (const std::exception& e) {
        out.frame = in.frame;
        out.error = QString("Template processing failed: %1").arg(e.what());
        return false;
    }
}

QVariantMap TemplateModule::getParameters() const
{
    QVariantMap params;
    params["example_parameter"] = m_exampleParameter;
    return params;
}

void TemplateModule::setParameters(const QVariantMap& params)
{
    // Clamp the example parameter to a valid range
    if (!params.contains("example_parameter")) {
        return;
    }

    int value = params.value("example_parameter").toInt();
    if (value < 1) {
        value = 1;
    }
    m_exampleParameter = value;
}

} // namespace scopeone::core::internal
