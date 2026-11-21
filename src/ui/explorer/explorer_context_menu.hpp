#pragma once

#include "ui/explorer/catalog_tree_model.hpp"

#include <QMenu>

namespace capybaradb::ui {

struct ExplorerContextCallbacks {
    std::function<void()>                              on_connect;
    std::function<void(const QModelIndex&)>            on_disconnect_server;
    std::function<void(const QModelIndex&)>            on_new_query;
    std::function<void(const QModelIndex&, uint32_t)>  on_select_top;
    std::function<void(const QModelIndex&, bool)>      on_script_select; // true=new query, false=clipboard
    std::function<void(const QModelIndex&)>            on_filter_settings;
    std::function<void(const QModelIndex&)>            on_remove_filter;
    std::function<void(const QModelIndex&)>            on_refresh;
};

[[nodiscard]] QMenu* build_explorer_context_menu(
    const CatalogTreeNode* node,
    const QModelIndex& index,
    uint32_t select_top_rows,
    const ExplorerContextCallbacks& callbacks,
    QWidget* parent);

} // namespace capybaradb::ui
