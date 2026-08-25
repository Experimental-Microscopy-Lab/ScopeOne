#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtPlugin>
#include <memory>
#include <utility>
#include <variant>

namespace scopeone::core
{
    enum class ProcessingParameterType
    {
        Integer,
        Real,
        Boolean,
        Choice
    };

    struct ProcessingParameterChoice
    {
        QString name;
        QVariant value;
    };

    struct ProcessingParameterDescriptor
    {
        QString key;
        QString name;
        ProcessingParameterType type{ProcessingParameterType::Integer};
        QVariant defaultValue;
        QVariant minimum;
        QVariant maximum;
        QVariant step;
        int decimals{0};
        QList<ProcessingParameterChoice> choices;
    };

    struct ProcessingModuleDescriptor
    {
        QString id;
        QString name;
        int schemaVersion{1};
        QList<ProcessingParameterDescriptor> parameters;
        bool resettable{false};
    };

    struct ComplexFrame
    {
        QString sourceId;
        int width{0};
        int height{0};
        int stride{0};
        int sourceWidth{0};
        int sourceHeight{0};
        quint64 frameIndex{0};
        quint64 timestampNs{0};
        QByteArray real;
        QByteArray imaginary;

        bool isValid() const
        {
            const qint64 samples = static_cast<qint64>(stride) * height;
            const qint64 bytes = samples * static_cast<qint64>(sizeof(float));
            return width > 0 && height > 0 && stride >= width && bytes > 0
                && real.size() == bytes && imaginary.size() == bytes;
        }
    };

    using ProcessingValue = std::variant<ImageFrame, ComplexFrame>;

    struct ProcessingResult
    {
        ImageFrame frame;
        ProcessingValue value;
        QString error;

        ProcessingResult() = default;
        ProcessingResult(const ImageFrame& output, QString message = {})
            : frame(output), value(output), error(std::move(message)) {}
        ProcessingResult(ImageFrame&& output, QString message = {})
            : frame(output), value(std::move(output)), error(std::move(message)) {}
        ProcessingResult(ProcessingValue output, QString message = {})
            : value(std::move(output)), error(std::move(message))
        {
            if (std::holds_alternative<ImageFrame>(value))
            {
                frame = std::get<ImageFrame>(value);
            }
        }

        bool succeeded() const
        {
            if (!error.isEmpty())
            {
                return false;
            }
            if (std::holds_alternative<ComplexFrame>(value))
            {
                return std::get<ComplexFrame>(value).isValid();
            }
            return std::get<ImageFrame>(value).isValid();
        }

        bool hasImage() const
        {
            return std::holds_alternative<ImageFrame>(value)
                && std::get<ImageFrame>(value).isValid();
        }
    };

    class SCOPEONE_CORE_EXPORT ProcessingModule
    {
    public:
        virtual ~ProcessingModule() = default;

        virtual QString id() const = 0;
        virtual QString name() const = 0;
        virtual QVariantMap parameters() const = 0;
        virtual void setParameters(const QVariantMap& parameters) = 0;
        virtual std::unique_ptr<ProcessingModule> createRuntime() const = 0;
        virtual bool resetState() { return false; }
        virtual ProcessingResult process(const ImageFrame& frame, int processingBitDepth) = 0;

        virtual ProcessingResult processValue(const ProcessingValue& input,
                                              int processingBitDepth)
        {
            if (!std::holds_alternative<ImageFrame>(input))
            {
                return ProcessingResult(ImageFrame{}, QStringLiteral("Module requires an image input"));
            }
            return process(std::get<ImageFrame>(input), processingBitDepth);
        }
    };

    class ProcessingPlugin
    {
    public:
        virtual ~ProcessingPlugin() = default;

        virtual QList<ProcessingModuleDescriptor> processingModules() const = 0;
        virtual std::unique_ptr<ProcessingModule> createProcessingModule(const QString& moduleId) = 0;
    };
}

#define ScopeOneProcessingPlugin_iid "org.scopeone.ProcessingPlugin/1.0"
Q_DECLARE_INTERFACE(scopeone::core::ProcessingPlugin, ScopeOneProcessingPlugin_iid)
