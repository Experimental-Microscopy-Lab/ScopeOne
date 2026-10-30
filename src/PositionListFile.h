#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace scopeone::ui
{
    // One stage position of a position list
    struct ListedPosition
    {
        QString label;
        double x{0.0};
        double y{0.0};
        std::optional<double> z;
        int gridRow{0};
        int gridColumn{0};
    };

    // Reads and writes position lists in the format of the Micro-Manager PositionList, a
    // JSON property map that Micro-Manager saves with the suffix .pos
    class PositionListFile
    {
    public:
        static bool save(const QString& path,
                         const QList<ListedPosition>& positions,
                         const QString& xyStage,
                         const QString& focusStage,
                         QString* errorMessage = nullptr);
        static bool load(const QString& path,
                         QList<ListedPosition>& positions,
                         QString* errorMessage = nullptr);
    };
}
