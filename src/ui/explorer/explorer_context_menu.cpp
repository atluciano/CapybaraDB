#include "ui/explorer/explorer_context_menu.hpp"

#include <QAction>
#include <QFont>

namespace capybaradb::ui {

QMenu* build_explorer_context_menu(
    const CatalogTreeNode* node,
    const QModelIndex& index,
    uint32_t select_top_rows,
    const ExplorerContextCallbacks& callbacks,
    QWidget* parent) {
    auto* menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto add_sep = [&]() {
        if (!menu->actions().isEmpty() && !menu->actions().constLast()->isSeparator()) {
            menu->addSeparator();
        }
    };

    if (node == nullptr) {
        QAction* act = menu->addAction(QStringLiteral("&Connect…"));
        QObject::connect(act, &QAction::triggered, parent, [cb = callbacks.on_connect]() {
            if (cb) cb();
        });
        return menu;
    }

    using drivers::CatalogKind;
    const drivers::CatalogNodeData& d = node->data;

    if (d.kind == CatalogKind::Placeholder) {
        QAction* act = menu->addAction(QStringLiteral("&Refresh"));
        QObject::connect(act, &QAction::triggered, parent, [cb = callbacks.on_refresh, index]() {
            if (cb) cb(index.parent());
        });
        return menu;
    }

    if (d.kind == CatalogKind::Server) {
        QAction* c = menu->addAction(QStringLiteral("&Connect…"));
        QObject::connect(c, &QAction::triggered, parent, [cb = callbacks.on_connect]() {
            if (cb) cb();
        });

        QAction* dc = menu->addAction(QStringLiteral("&Disconnect"));
        QObject::connect(dc, &QAction::triggered, parent, [cb = callbacks.on_disconnect_server, index]() {
            if (cb) cb(index);
        });

        add_sep();
        QAction* nq = menu->addAction(QStringLiteral("New &Query"));
        QObject::connect(nq, &QAction::triggered, parent, [cb = callbacks.on_new_query, index]() {
            if (cb) cb(index);
        });
    } else if (d.kind == CatalogKind::Relation &&
               (d.relation_type == 'r' || d.relation_type == 'p' || d.relation_type == 'v' || d.relation_type == 'm')) {
        const uint32_t top_n = select_top_rows > 0 ? select_top_rows : 1000u;
        QAction* st = menu->addAction(QStringLiteral("Select Top %1 Rows").arg(top_n));
        QFont bold = st->font();
        bold.setBold(true);
        st->setFont(bold);
        QObject::connect(st, &QAction::triggered, parent, [cb = callbacks.on_select_top, index, top_n]() {
            if (cb) cb(index, top_n);
        });

        auto* script_menu = menu->addMenu(
            d.relation_type == 'v' || d.relation_type == 'm'
                ? QStringLiteral("Script &View as")
                : QStringLiteral("Script &Table as"));
        auto* select_menu = script_menu->addMenu(QStringLiteral("SELECT To"));
        QAction* sq = select_menu->addAction(QStringLiteral("New Query Editor Window"));
        QObject::connect(sq, &QAction::triggered, parent, [cb = callbacks.on_script_select, index]() {
            if (cb) cb(index, true);
        });
        QAction* sc = select_menu->addAction(QStringLiteral("Clipboard"));
        QObject::connect(sc, &QAction::triggered, parent, [cb = callbacks.on_script_select, index]() {
            if (cb) cb(index, false);
        });

        QAction* nq = menu->addAction(QStringLiteral("New &Query"));
        QObject::connect(nq, &QAction::triggered, parent, [cb = callbacks.on_new_query, index]() {
            if (cb) cb(index);
        });
    } else if (!d.is_offline) {
        QAction* nq = menu->addAction(QStringLiteral("New &Query"));
        QObject::connect(nq, &QAction::triggered, parent, [cb = callbacks.on_new_query, index]() {
            if (cb) cb(index);
        });
    }

    if (d.can_filter()) {
        add_sep();
        auto* filter_menu = menu->addMenu(QStringLiteral("&Filter"));
        QAction* fs = filter_menu->addAction(QStringLiteral("&Filter Settings…"));
        QObject::connect(fs, &QAction::triggered, parent, [cb = callbacks.on_filter_settings, index]() {
            if (cb) cb(index);
        });
        QAction* rf = filter_menu->addAction(QStringLiteral("&Remove Filter"));
        rf->setEnabled(node->filter.is_active());
        QObject::connect(rf, &QAction::triggered, parent, [cb = callbacks.on_remove_filter, index]() {
            if (cb) cb(index);
        });
    }

    add_sep();
    QAction* ref = menu->addAction(QStringLiteral("&Refresh"));
    QObject::connect(ref, &QAction::triggered, parent, [cb = callbacks.on_refresh, index]() {
        if (cb) cb(index);
    });

    return menu;
}

} // namespace capybaradb::ui
