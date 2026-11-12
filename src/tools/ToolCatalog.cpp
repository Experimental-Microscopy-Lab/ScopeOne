#include "tools/ToolContext.h"

#include <algorithm>

namespace scopeone::ui
{
    namespace
    {
        // Built on first use, so a tool registering from the static initialization of any source
        // file finds it ready whatever the order of initialization is
        std::vector<ToolDefinition>& catalog()
        {
            static std::vector<ToolDefinition> tools;
            return tools;
        }

        QString sortKey(const QString& text)
        {
            return QString(text).remove(QLatin1Char('&'));
        }
    }

    void registerToolDefinition(ToolDefinition definition)
    {
        catalog().push_back(std::move(definition));
    }

    std::vector<ToolDefinition> registeredTools()
    {
        std::vector<ToolDefinition> tools = catalog();
        std::stable_sort(tools.begin(), tools.end(), [](const ToolDefinition& left, const ToolDefinition& right)
        {
            const QString leftCategory = sortKey(left.descriptor.category);
            const QString rightCategory = sortKey(right.descriptor.category);
            if (leftCategory.isEmpty() != rightCategory.isEmpty())
            {
                return leftCategory.isEmpty();
            }
            const int category = leftCategory.compare(rightCategory, Qt::CaseInsensitive);
            if (category != 0)
            {
                return category < 0;
            }
            return sortKey(left.descriptor.name).compare(sortKey(right.descriptor.name), Qt::CaseInsensitive) < 0;
        });
        return tools;
    }
}
