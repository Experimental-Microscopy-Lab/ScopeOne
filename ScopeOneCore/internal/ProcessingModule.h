#pragma once

#include <QString>
#include <QVariantMap>
#include <memory>

#include "scopeone/ExperimentDocument.h"

namespace scopeone::core::internal
{
    using ImageFrame = scopeone::core::ImageFrame;
    using ProcessingModuleKind = scopeone::core::ProcessingModuleKind;

    struct ProcessingResult
    {
        ImageFrame frame;
        QString error;

        bool succeeded() const { return error.isEmpty() && frame.isValid(); }
    };

    class ProcessingModule
    {
    public:
        virtual ~ProcessingModule() = default;

        virtual ProcessingModuleKind kind() const noexcept = 0;
        virtual QString name() const = 0;
        virtual QVariantMap parameters() const = 0;
        virtual void setParameters(const QVariantMap& params) = 0;
        virtual std::unique_ptr<ProcessingModule> createRuntime() const = 0;
        virtual bool resetState() { return false; }
        virtual ProcessingResult process(const ImageFrame& frame, int processingBitDepth) = 0;
    };
}
