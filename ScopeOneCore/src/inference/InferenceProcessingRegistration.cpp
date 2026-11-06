#include "internal/inference/OnnxInferenceModule.h"

#include "internal/ProcessingModuleRegistry.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>

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

            // The CPU package of ONNX Runtime has no CUDA provider, and offering CUDA there only
            // failed when the model was loaded
            QList<core::ProcessingParameterChoice> providers{{QStringLiteral("CPU"), 0}};
            const std::vector<std::string> available = Ort::GetAvailableProviders();
            if (std::find(available.begin(), available.end(), "CUDAExecutionProvider") != available.end())
            {
                providers.append({QStringLiteral("CUDA"), 1});
            }

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
           providers},
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

        // The descriptor asks ONNX Runtime for its providers, so it is made when a registry is
        // built and not while Core is loaded
        void registerInferenceProcessingModules(core::internal::ProcessingModuleRegistry& registry)
        {
            for (const core::ProcessingModuleDescriptor& descriptor : descriptors())
            {
                const QString id = descriptor.id;
                registry.registerBuiltIn(descriptor, [id]() { return create(id); });
            }
        }

        [[maybe_unused]] const bool registered =
            core::internal::addProcessingModuleRegistration(registerInferenceProcessingModules);
    }
}
