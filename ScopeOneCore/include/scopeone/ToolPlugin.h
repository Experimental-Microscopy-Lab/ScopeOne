#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QtPlugin>
#include <memory>

class QWidget;

namespace scopeone::ui
{
    enum class ToolWindowMode
    {
        Modal,
        ModelessSingleton
    };

    struct ToolDescriptor
    {
        QString id;
        QString name;
        QString category;
        ToolWindowMode windowMode{ToolWindowMode::ModelessSingleton};
        bool requiresCamera{false};
    };

    class ScopeOneToolContext
    {
    public:
        virtual ~ScopeOneToolContext() = default;

        virtual scopeone::core::ScopeOneCore& core() const = 0;
        virtual QString currentLayerKey() const = 0;
        virtual scopeone::core::ImageFrame currentFrame() const = 0;
        virtual scopeone::core::ImageFrame publishToolStreamFrame(
            const QString& sourceId,
            const scopeone::core::ImageFrame& frame,
            const QString& displayName = QString()) = 0;
        virtual void showLayers(const QStringList& layerKeys, bool sideBySide = false) = 0;
        virtual void showToolStatus(const QString& message, int timeoutMs = 5000) = 0;
        virtual void presentSession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& title) = 0;
    };

    class ScopeOneToolPlugin
    {
    public:
        virtual ~ScopeOneToolPlugin() = default;

        virtual QList<ToolDescriptor> tools() const = 0;
        virtual QWidget* createTool(const QString& toolId,
                                    ScopeOneToolContext& context,
                                    QWidget* parent) = 0;
    };
}

#define ScopeOneToolPlugin_iid "org.scopeone.ToolPlugin/1.0"
Q_DECLARE_INTERFACE(scopeone::ui::ScopeOneToolPlugin, ScopeOneToolPlugin_iid)
