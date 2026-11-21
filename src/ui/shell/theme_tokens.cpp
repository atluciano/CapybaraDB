#include "ui/shell/theme_tokens.hpp"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QPalette>
#include <QStringList>
#include <QStyleFactory>

static void init_capybaradb_theme_resources() {
    Q_INIT_RESOURCE(resources);
}

namespace capybaradb::ui::theme {
namespace {

QString pick_first_available_family(std::initializer_list<const char*> candidates, const QString& fallback) {
    const QStringList families = QFontDatabase::families();
    for (const char* candidate : candidates) {
        const QString name = QString::fromLatin1(candidate);
        if (families.contains(name, Qt::CaseInsensitive)) {
            return name;
        }
    }
    return fallback;
}

} // namespace

QFont resolve_ui_font(int pixel_size) {
#ifdef _WIN32
    QFont font(QStringLiteral("Segoe UI"));
#else
    const QString family = pick_first_available_family(
        {"Inter", "Liberation Sans", "Noto Sans"},
        QStringLiteral("Sans Serif"));
    QFont font(family);
#endif
    font.setPixelSize(pixel_size);
    return font;
}

QFont resolve_mono_font(int pixel_size) {
#ifdef _WIN32
    QFont font(QStringLiteral("Consolas"));
#else
    const QString family = pick_first_available_family(
        {"JetBrains Mono", "Liberation Mono", "DejaVu Sans Mono"},
        QStringLiteral("Monospace"));
    QFont font(family);
#endif
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPixelSize(pixel_size);
    return font;
}

QString load_classic_blue_stylesheet() {
    init_capybaradb_theme_resources();
    QFile file(QStringLiteral(":/themes/classic_blue.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool apply_classic_blue_theme(QApplication& app) {
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }
    app.setFont(resolve_ui_font(kUiFontSizePx));

    QPalette pal = app.palette();
    pal.setColor(QPalette::Window, color(kShellBg));
    pal.setColor(QPalette::WindowText, color(kPrimaryText));
    pal.setColor(QPalette::Base, QColor(Qt::white));
    pal.setColor(QPalette::AlternateBase, color(kGridCellBg));
    pal.setColor(QPalette::Text, color(kPrimaryText));
    pal.setColor(QPalette::Button, color(kToolbarBg));
    pal.setColor(QPalette::ButtonText, color(kPrimaryText));
    pal.setColor(QPalette::Highlight, color(kTreeSelectionActiveBg));
    pal.setColor(QPalette::HighlightedText, QColor(Qt::white));
    pal.setColor(QPalette::ToolTipBase, color(kPlanTooltipBg));
    pal.setColor(QPalette::ToolTipText, color(kPrimaryText));
    app.setPalette(pal);

    const QString qss = load_classic_blue_stylesheet();
    if (qss.isEmpty()) {
        return false;
    }
    app.setStyleSheet(qss);
    return true;
}

} // namespace capybaradb::ui::theme
