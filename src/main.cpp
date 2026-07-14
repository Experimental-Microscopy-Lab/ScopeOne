#include <QApplication>
#include <QIcon>
#include <memory>
#include "AppVersion.h"
#include "scopeone/ScopeOneCore.h"
#include "ConsoleWidget.h"
#include "MainWindow.h"

// Create the shared core before the main window
int main(int argc, char* argv[])
{
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
