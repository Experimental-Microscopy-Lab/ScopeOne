#include "Appearance.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>

namespace scopeone::ui
{
    void applyWidgetStyle(const QString& widgetStyle)
    {
        if (widgetStyle != QStringLiteral("default") && !widgetStyle.isEmpty())
        {
            QApplication::setStyle(QStyleFactory::create(widgetStyle));
        }
    }

    void applyColorScheme(const QString& colorScheme)
    {
        QStyleHints* styleHints = QApplication::styleHints();
        if (colorScheme == QStringLiteral("light"))
        {
            styleHints->setColorScheme(Qt::ColorScheme::Light);
        }
        else if (colorScheme == QStringLiteral("dark"))
        {
            styleHints->setColorScheme(Qt::ColorScheme::Dark);
        }
        else
        {
            styleHints->unsetColorScheme();
        }
    }

    void applyStoredAppearance()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        applyWidgetStyle(settings.value(QStringLiteral("Appearance/Style"), QStringLiteral("default")).toString());
        applyColorScheme(settings.value(QStringLiteral("Appearance/ColorScheme"), QStringLiteral("system")).toString());
        // A change of the scheme reaches the widgets as an event, which is handled here and not
        // after the widgets exist
        QCoreApplication::processEvents();
    }
}
