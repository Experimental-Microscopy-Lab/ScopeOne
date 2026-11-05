#include "internal/inference/InferenceProcessingRegistration.h"
#include "internal/inference/OnnxInferenceModule.h"

#include "internal/ProcessingModuleRegistry.h"

namespace scopeone::core::internal::inference
{
    namespace
    {
        QList<core::ProcessingModuleDescriptor> descriptors()
        {
            core::ProcessingParameterDescriptor modelPath{
                QStringLiteral("model_path"),
                QStringLiteral("Model"),
                core::ProcessingParameterType::FilePath,
                QString{}};
            modelPath.fileFilter = QStringLiteral("ONNX models (*.onnx)");

            return {{QStringLiteral("onnx_inference"),
         QStringLiteral("ONNX Inference"),
         2,
         {modelPath,
          {QStringLiteral("provider"),
           QStringLiteral("Provider"),
           scopeone::core::ProcessingParameterType::Choice,
           0,
           {},
           {},
           {},
           0,
           {{QStringLiteral("CPU"), 0}, {QStringLiteral("CUDA"), 1}}},
          {QStringLiteral("layout"),
           QStringLiteral("Layout"),
           scopeone::core::ProcessingParameterType::Choice,
           0,
           {},
           {},
           {},
           0,
           {{QStringLiteral("Auto"), 0},
            {QStringLiteral("NCHW"), 1},
            {QStringLiteral("NHWC"), 2}}},
          {QStringLiteral("input_scale"),
           QStringLiteral("Input scale"),
           scopeone::core::ProcessingParameterType::Real,
           1.0,
           0.000001,
           1000000.0,
           0.01,
           6},
          {QStringLiteral("input_mean"),
           QStringLiteral("Input mean"),
           scopeone::core::ProcessingParameterType::Real,
           0.0,
           -1000000.0,
           1000000.0,
           0.01,
           6},
          {QStringLiteral("input_std"),
           QStringLiteral("Input std"),
           scopeone::core::ProcessingParameterType::Real,
           1.0,
           0.000001,
           1000000.0,
           0.01,
           6},
          {QStringLiteral("output_mode"),
           QStringLiteral("Output"),
           scopeone::core::ProcessingParameterType::Choice,
           0,
           {},
           {},
           {},
           0,
           {{QStringLiteral("Image"), 0},
            {QStringLiteral("Input - Output"), 1}}}}}};
        }

        std::unique_ptr<core::ProcessingModule> create(const QString& moduleId)
        {
            if (moduleId == QStringLiteral("onnx_inference"))
            {
                return std::make_unique<OnnxInferenceModule>();
            }
            return {};
        }
    }

    void registerInferenceProcessingModules(core::internal::ProcessingModuleRegistry& registry)
    {
        for (const core::ProcessingModuleDescriptor& descriptor : descriptors())
        {
            const QString id = descriptor.id;
            registry.registerModule(descriptor, [id]() { return create(id); });
        }
    }
}
