#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>
#include <memory>
#include "AppVersion.h"
#include "scopeone/ScopeOneCore.h"
#include "ConsoleWidget.h"
#include "MainWindow.h"
#include "PluginManagerDialog.h"

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    // macOS supports OpenGL only up to 4.1, and ScopeOne currently uses no features introduced after 4.1
    format.setVersion(4, 1);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(SCOPEONE_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(SCOPEONE_APP_VERSION_STRING));
    app.setStyle(QStringLiteral("Fusion"));
    // Rounded, quiet controls that follow the colors of the palette, so they suit the light and dark themes
    app.setStyleSheet(QStringLiteral(R"(
        QToolTip { padding: 4px 6px; border: 1px solid palette(mid); border-radius: 4px; }
        QGroupBox { border: 1px solid palette(mid); border-radius: 6px; margin-top: 10px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QPushButton { padding: 4px 12px; border: 1px solid palette(mid); border-radius: 5px; background: palette(button); }
        QPushButton:hover { border-color: palette(highlight); }
        QPushButton:pressed { background: palette(midlight); }
        QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); }
        QPushButton:disabled { border-color: palette(midlight); }
        QPushButton:flat { border: none; background: transparent; }
        QToolBar { border: none; spacing: 4px; padding: 2px; }
        QToolButton { padding: 3px 6px; border: 1px solid transparent; border-radius: 5px; }
        QToolButton:hover { background: palette(midlight); border-color: palette(mid); }
        QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }
        QStatusBar::item { border: none; }
    )"));
    app.setWindowIcon(QIcon(":/Scopeone_Icon.svg"));
    scopeone::ui::ConsoleWidget::installQtMessageHandler();

    auto scopeOneCore = std::make_unique<scopeone::core::ScopeOneCore>();
    for (const QString& error : scopeone::ui::loadConfiguredHardwarePlugins(*scopeOneCore))
    {
        qWarning().noquote() << QStringLiteral("Failed to load hardware plugin %1").arg(error);
    }
    scopeone::ui::MainWindow window(scopeOneCore.get());
    window.show();
    return app.exec();
}
