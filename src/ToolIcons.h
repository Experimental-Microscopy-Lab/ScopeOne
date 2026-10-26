#pragma once

#include <QIcon>
#include <QPalette>

namespace scopeone::ui
{
    enum class ToolIconKind
    {
        Select,
        Rectangle,
        Ellipse,
        Polygon,
        Freehand,
        Line,
        Point,
        Wand,
        Open,
        Live,
        Snap,
        Contrast,
        Record
    };

    // A line drawing of a selection tool, in the colors of the palette so that it follows the
    // light and dark themes. The drawing is made at the size of the screen, so it stays sharp.
    QIcon toolIcon(ToolIconKind kind, const QPalette& palette, qreal devicePixelRatio, int size = 20);
}
