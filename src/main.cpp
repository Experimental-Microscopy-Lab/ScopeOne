#include <QApplication>
#include <QIcon>
#include <memory>
#include "scopeone/ScopeOneCore.h"
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    // Create the shared core before the main window
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));
    app.setWindowIcon(QIcon(":/Scopeone_Icon.svg"));

    auto scopeOneCore = std::make_unique<scopeone::core::ScopeOneCore>();
    scopeone::ui::MainWindow window(scopeOneCore.get());
    window.show();
    return app.exec();
}
