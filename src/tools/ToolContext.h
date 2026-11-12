#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace scopeone::core
{
    class ScopeOneCore;
    class RecordingSessionData;
}

// The tools of the Tools menu live in src/tools. A tool uses only the public Core headers and the
// context below, and registers itself with SCOPEONE_REGISTER_TOOL, so adding one changes no
// other file.
namespace scopeone::ui
{
    enum class ToolWindowMode
    {
        Modal,
        ModelessSingleton
    };

    // The name and the category are untranslated source texts in the "Tools" context, marked with
    // QT_TRANSLATE_NOOP, because tools register before the translations are loaded
    struct ToolDescriptor
    {
        QString id;
        QString name;
        QString category;
        ToolWindowMode windowMode{ToolWindowMode::ModelessSingleton};
        bool requiresCamera{false};
    };

    // What the desktop offers a tool beside the Core
    class ToolContext
    {
    public:
        virtual ~ToolContext() = default;

        virtual scopeone::core::ScopeOneCore& core() const = 0;
        virtual QString currentLayerKey() const = 0;
        virtual void showLayers(const QStringList& layerKeys, bool sideBySide = false) = 0;
        virtual void showToolStatus(const QString& message, int timeoutMs = 5000) = 0;
        virtual void presentSession(
            const std::shared_ptr<scopeone::core::RecordingSessionData>& session,
            const QString& title) = 0;
    };

    using ToolFactory = std::function<QWidget*(ToolContext& context, QWidget* parent)>;

    struct ToolDefinition
    {
        ToolDescriptor descriptor;
        ToolFactory create;
    };

    // Every registered tool, in the order the menu lists them: tools without a category first,
    // then by category and name
    std::vector<ToolDefinition> registeredTools();
    void registerToolDefinition(ToolDefinition definition);
}

// Registers the tool of a source file, when the program starts, with a descriptor and a factory:
// SCOPEONE_REGISTER_TOOL({id, name, category, mode, requiresCamera}, factory). Only these are
// stored then; the window is created when the user opens the tool. A file registers one tool.
#define SCOPEONE_REGISTER_TOOL(...)                                                              \
    namespace                                                                                    \
    {                                                                                            \
        [[maybe_unused]] const int toolRegistered =                                              \
            (::scopeone::ui::registerToolDefinition({__VA_ARGS__}), 0);                          \
    }
