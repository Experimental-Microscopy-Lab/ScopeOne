#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtPlugin>
#include <memory>

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

    struct ProcessingResult
    {
        ImageFrame frame;
        QString error;

        bool succeeded() const { return error.isEmpty() && frame.isValid(); }
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
