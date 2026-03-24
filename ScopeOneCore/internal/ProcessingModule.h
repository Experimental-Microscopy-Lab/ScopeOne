#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "scopeone/ImageFrame.h"

namespace scopeone::core::internal {

using ImageFrame = scopeone::core::ImageFrame;

struct ModuleInput
{
    ImageFrame frame;
    ModuleInput() = default;

    explicit ModuleInput(const ImageFrame& f)
        : frame(f) {}
};

struct ModuleOutput
{
    ImageFrame frame;
    QString error;

    bool hasError() const { return !error.isEmpty(); }
    bool isValid() const { return frame.isValid() && !hasError(); }
};

class ProcessingModule : public QObject
{
    Q_OBJECT

public:
    explicit ProcessingModule(QObject* parent = nullptr);
    virtual ~ProcessingModule() = default;

    virtual bool process(const ModuleInput& in, ModuleOutput& out) = 0;

    virtual QString getModuleName() const = 0;

    virtual QVariantMap getParameters() const = 0;
    virtual void setParameters(const QVariantMap& params) = 0;
};

}
