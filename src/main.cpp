#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>
#include <memory>
#include "AppVersion.h"
#include "scopeone/ScopeOneCore.h"
#include "ConsoleWidget.h"
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    // macOS supports OpenGL only up to 4.1, and ScopeOne currently uses no features introduced after 4.1
    format.setVersion(4, 1);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(SCOPEONE_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(SCOPEONE_APP_VERSION_STRING));
    app.setStyle(QStringLiteral("Fusion"));
    app.setWindowIcon(QIcon(":/Scopeone_Icon.svg"));
    scopeone::ui::ConsoleWidget::installQtMessageHandler();

    auto scopeOneCore = std::make_unique<scopeone::core::ScopeOneCore>();
    scopeone::ui::MainWindow window(scopeOneCore.get());
    window.show();
    return app.exec();
}
