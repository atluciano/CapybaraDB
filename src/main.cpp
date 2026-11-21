#include "ui/shell/main_window.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QApplication>
#include <QStringList>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("CapybaraDB"));
    app.setOrganizationName(QStringLiteral("CapybaraDB"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    capybaradb::ui::theme::apply_classic_blue_theme(app);

    capybaradb::ui::MainWindow window;
    window.show();

    if (!app.arguments().contains(QStringLiteral("--no-startup-dialog"))) {
        QTimer::singleShot(0, &window, &capybaradb::ui::MainWindow::prompt_connect_at_startup);
    }

    return app.exec();
}
