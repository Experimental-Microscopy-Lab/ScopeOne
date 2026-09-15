#include "IscatProcessingModule.h"

#include "scopeone/ProcessingPlugin.h"

#include <initializer_list>
#include <QObject>

namespace
{
    scopeone::core::ProcessingParameterDescriptor makeRealParameter(
        const char* key,
        const char* name,
        double value,
        double minimum,
        double maximum,
        double step,
        int decimals)
    {
        scopeone::core::ProcessingParameterDescriptor descriptor{
            QString::fromLatin1(key),
            QString::fromLatin1(name),
            scopeone::core::ProcessingParameterType::Real,
            value,
            minimum,
            maximum,
            step};
        descriptor.decimals = decimals;
        return descriptor;
    }

    scopeone::core::ProcessingParameterDescriptor makeBooleanParameter(
        const char* key,
        const char* name,
        bool value)
    {
        return {QString::fromLatin1(key),
                QString::fromLatin1(name),
                scopeone::core::ProcessingParameterType::Boolean,
                value};
    }

    scopeone::core::ProcessingParameterDescriptor makeIntegerParameter(
        const char* key,
        const char* name,
        int value,
        int minimum,
        int maximum,
        int step)
    {
        return {QString::fromLatin1(key),
                QString::fromLatin1(name),
                scopeone::core::ProcessingParameterType::Integer,
                value,
                minimum,
                maximum,
                step};
    }

    scopeone::core::ProcessingParameterDescriptor makeChoiceParameter(
        const char* key,
        const char* name,
        int value,
        std::initializer_list<const char*> choices)
    {
        scopeone::core::ProcessingParameterDescriptor descriptor{
            QString::fromLatin1(key),
            QString::fromLatin1(name),
            scopeone::core::ProcessingParameterType::Choice,
            value};
        int index = 0;
        for (const char* choice : choices)
        {
            descriptor.choices.append({QString::fromLatin1(choice), index++});
        }
        return descriptor;
    }

    class IscatProcessingPlugin final : public QObject,
                                         public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        QList<scopeone::core::ProcessingModuleDescriptor> processingModules() const override
        {
            return {{QStringLiteral("iscat.processing"),
                     QStringLiteral("iSCAT Processing"),
                     1,
                     {makeChoiceParameter("mode",
                                           "Background mode",
                                           0,
                                           {"Flat Field",
                                            "Temporal Median",
                                            "Dynamic EMA",
                                            "Snapshot Reference",
                                            "Differential Rolling (DRA)"}),
                      makeRealParameter("blur_sigma", "Blur sigma", 15.0, 1.0, 100.0, 0.5, 1),
                      makeIntegerParameter("median_window", "Median window", 31, 3, 101, 2),
                      makeRealParameter("ema_alpha", "Smoothing factor", 0.05, 0.001, 0.5, 0.005, 3),
                      makeBooleanParameter("capture_reference", "Capture Reference", false),
                      makeIntegerParameter("batch_size", "DRA batch size", 16, 1, 500, 1),
                      makeRealParameter("contrast_gain", "Contrast gain", 20.0, 1.0, 500.0, 1.0, 1),
                      makeBooleanParameter("enable_high_pass", "Enable High-Pass", false),
                      makeRealParameter("high_pass_sigma", "High-Pass sigma", 30.0, 2.0, 200.0, 1.0, 1),
                      makeChoiceParameter("output_mode",
                                          "Output mode",
                                          0,
                                          {"Contrast Centered", "Absolute Contrast", "Estimated Background"})},
                     true}};
        }

        std::unique_ptr<scopeone::core::ProcessingModule> createProcessingModule(
            const QString& moduleId) override
        {
            return moduleId == QStringLiteral("iscat.processing")
                       ? std::make_unique<scopeone::iscat::IscatProcessingModule>()
                       : nullptr;
        }
    };
}

#include "IscatProcessingPlugin.moc"
