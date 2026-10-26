#pragma once

#include <QString>

namespace scopeone::ui
{
    // The Qt widget style by name, or the default one for "default"
    void applyWidgetStyle(const QString& widgetStyle);
    // The color scheme by name: light, dark, or the one of the system for anything else
    void applyColorScheme(const QString& colorScheme);
    // The style and color scheme that the user chose, before any window exists, so that every widget
    // starts with the palette of the scheme
    void applyStoredAppearance();
}
