#pragma once

#include "internal/SignalSourceManager.h"

namespace scopeone::core::internal
{
    class PtuFileSourceFactory final : public SignalSourceFactory
    {
    public:
        QList<scopeone::core::SignalSourceDescriptor> signalSources() const override;
        scopeone::core::SignalSource* createSignalSource(
            const QString& sourceId,
            QObject* parent = nullptr) override;
    };
}
