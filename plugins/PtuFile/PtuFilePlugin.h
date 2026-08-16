#pragma once

#include "scopeone/SignalSource.h"

#include <QObject>

namespace scopeone::plugins
{
    class PtuFilePlugin final : public QObject,
                                public scopeone::core::SignalSourcePlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID SCOPEONE_SIGNAL_SOURCE_PLUGIN_IID)
        Q_INTERFACES(scopeone::core::SignalSourcePlugin)

    public:
        QList<scopeone::core::SignalSourceDescriptor> signalSources() const override;
        scopeone::core::SignalSource* createSignalSource(
            const QString& sourceId,
            QObject* parent = nullptr) override;
    };
}
