#include "OnnxInferenceModule.h"

#include "scopeone/ScopeOneCore.h"
#include "scopeone/inference/InferenceExport.h"

extern "C" SCOPEONE_INFERENCE_EXPORT void scopeone_register_processing_modules(
    scopeone::core::ScopeOneCore* core)
{
    scopeone::core::ProcessingParameterDescriptor modelPath{
        QStringLiteral("model_path"),
        QStringLiteral("Model"),
        scopeone::core::ProcessingParameterType::FilePath,
        QString{}};
    modelPath.fileFilter = QStringLiteral("ONNX models (*.onnx)");

    core->registerProcessingModule(
        {QStringLiteral("onnx_inference"),
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
            {QStringLiteral("Input - Output"), 1}}}}},
        []()
        {
            return std::make_unique<scopeone::inference::OnnxInferenceModule>();
        });
}
