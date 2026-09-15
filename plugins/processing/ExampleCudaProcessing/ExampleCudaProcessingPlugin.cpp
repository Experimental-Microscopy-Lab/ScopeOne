#include "scopeone/ProcessingPlugin.h"
#include "scopeone/cuda/CudaRealImageModule.h"

#include <QObject>

namespace example_cuda
{
    bool launchInvert(const void* input,
                      std::size_t inputPitchBytes,
                      void* output,
                      std::size_t outputPitchBytes,
                      int width,
                      int height);
}

namespace
{
    class InvertModule final : public scopeone::cuda::CudaRealImageModule
    {
    public:
        InvertModule()
            : CudaRealImageModule(scopeone::cuda::GpuMemoryLayout::Pitched2D)
        {
        }

        QString id() const override { return QStringLiteral("example.cuda_invert"); }
        QString name() const override { return QStringLiteral("CUDA Example Invert"); }
        QVariantMap parameters() const override { return {}; }
        void setParameters(const QVariantMap&) override {}

        std::unique_ptr<scopeone::core::ProcessingModule> createRuntime() const override
        {
            return std::make_unique<InvertModule>();
        }

    protected:
        bool processDevice(const scopeone::cuda::GpuRealFrame& input,
                           scopeone::cuda::GpuRealFrame& output,
                           int) override
        {
            return example_cuda::launchInvert(input.data(),
                                              input.pitchBytes(),
                                              output.data(),
                                              output.pitchBytes(),
                                              input.width(),
                                              input.height());
        }
    };

    class ExampleCudaProcessingPlugin final : public QObject,
                                               public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        QList<scopeone::core::ProcessingModuleDescriptor> processingModules() const override
        {
            return {{QStringLiteral("example.cuda_invert"),
                     QStringLiteral("CUDA Example Invert")}};
        }

        std::unique_ptr<scopeone::core::ProcessingModule> createProcessingModule(
            const QString& moduleId) override
        {
            return moduleId == QStringLiteral("example.cuda_invert")
                       ? std::make_unique<InvertModule>()
                       : nullptr;
        }
    };
}

#include "ExampleCudaProcessingPlugin.moc"
