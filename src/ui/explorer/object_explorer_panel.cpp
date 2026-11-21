#include "ui/explorer/object_explorer_panel.hpp"

#include "core/config/state_paths.hpp"
#include "ui/dialogs/filter_settings_dialog.hpp"
#include "ui/explorer/explorer_context_menu.hpp"
#include "ui/shell/icon_catalog.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_set>

namespace capybaradb::ui {

ObjectExplorerPanel::ObjectExplorerPanel(
    std::filesystem::path toolbar_prefs_path,
    core::CatalogWorkerPool* worker_pool,
    QWidget* parent)
    : QWidget(parent)
    , prefs_path_(toolbar_prefs_path.empty()
          ? capybaradb::core::explorer_toolbar_prefs_path()
          : std::move(toolbar_prefs_path))
    , worker_pool_(worker_pool) {
    setObjectName(QStringLiteral("ObjectExplorerPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    build_ui();
    load_toolbar_preferences();
}

ObjectExplorerPanel::~ObjectExplorerPanel() = default;

void ObjectExplorerPanel::build_ui() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // Explorer Toolbar (6 buttons + right-aligned "⋯" options button)
    auto* toolbar_container = new QWidget(this);
    toolbar_container->setObjectName(QStringLiteral("ExplorerToolbar"));
    toolbar_container->setAttribute(Qt::WA_StyledBackground, true);
    auto* tb_layout = new QHBoxLayout(toolbar_container);
    tb_layout->setContentsMargins(4, 3, 4, 2);
    tb_layout->setSpacing(1);

    auto make_btn = [&](const std::string& id, const QString& label, const QString& tooltip, const QString& gesture) -> QToolButton* {
        auto* btn = new QToolButton(toolbar_container);
        btn->setObjectName(QStringLiteral("ExplorerToolButton"));
        btn->setAutoRaise(true);
        btn->setToolTip(tooltip);
        btn->setAccessibleName(label);
        buttons_.push_back(ToolButtonEntry{id, label, gesture, btn});
        button_map_.emplace(id, btn);
        return btn;
    };

    auto* connect_text_btn = make_btn("Connect", QStringLiteral("Connect"), QStringLiteral("Connect"), QString{});
    connect_text_btn->setText(QStringLiteral("Connect"));
    connect_text_btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb_layout->addWidget(connect_text_btn);
    connect(connect_text_btn, &QToolButton::clicked, this, &ObjectExplorerPanel::connect_requested);

    auto* connect_plug_btn = make_btn("ConnectExplorer", QStringLiteral("Connect Object Explorer…"), QStringLiteral("Connect Object Explorer"), QString{});
    connect_plug_btn->setIcon(get_icon(IconId::Plug));
    connect_plug_btn->setIconSize(QSize(16, 16));
    tb_layout->addWidget(connect_plug_btn);
    connect(connect_plug_btn, &QToolButton::clicked, this, &ObjectExplorerPanel::connect_requested);

    auto* disconnect_btn = make_btn("Disconnect", QStringLiteral("Disconnect"), QStringLiteral("Disconnect"), QString{});
    disconnect_btn->setIcon(get_icon(IconId::PlugOff));
    disconnect_btn->setIconSize(QSize(16, 16));
    disconnect_btn->setEnabled(false);
    tb_layout->addWidget(disconnect_btn);
    connect(disconnect_btn, &QToolButton::clicked, this, [this]() {
        disconnect_all();
        emit disconnect_requested();
    });

    auto* stop_btn = make_btn("Stop", QStringLiteral("Stop"), QStringLiteral("Stop"), QString{});
    stop_btn->setIcon(get_icon(IconId::StopIdle));
    stop_btn->setIconSize(QSize(16, 16));
    stop_btn->setEnabled(false);
    tb_layout->addWidget(stop_btn);
    connect(stop_btn, &QToolButton::clicked, this, [this]() {
        stop_selected_load();
        emit stop_requested();
    });

    auto* filter_btn = make_btn("Filter", QStringLiteral("Filter"), QStringLiteral("Filter"), QString{});
    filter_btn->setIcon(get_icon(IconId::Filter));
    filter_btn->setIconSize(QSize(16, 16));
    filter_btn->setEnabled(false);
    tb_layout->addWidget(filter_btn);
    connect(filter_btn, &QToolButton::clicked, this, [this]() {
        open_filter_for_selected();
        emit filter_requested();
    });

    auto* refresh_btn = make_btn("Refresh", QStringLiteral("Refresh"), QStringLiteral("Refresh (F5)"), QStringLiteral("F5"));
    refresh_btn->setIcon(get_icon(IconId::Refresh));
    refresh_btn->setIconSize(QSize(16, 16));
    tb_layout->addWidget(refresh_btn);
    connect(refresh_btn, &QToolButton::clicked, this, [this]() {
        refresh_selected();
        emit refresh_requested();
    });

    tb_layout->addStretch(1);

    options_button_ = new QToolButton(toolbar_container);
    options_button_->setObjectName(QStringLiteral("ExplorerToolbarOptionsButton"));
    options_button_->setText(QStringLiteral("⋯"));
    options_button_->setToolTip(QStringLiteral("Object Explorer Toolbar Options"));
    options_button_->setAccessibleName(QStringLiteral("Object Explorer Toolbar Options"));
    options_button_->setAutoRaise(true);
    tb_layout->addWidget(options_button_);
    connect(options_button_, &QToolButton::clicked, this, &ObjectExplorerPanel::open_toolbar_options_menu);

    root_layout->addWidget(toolbar_container);

    // TreeView backed by lazy-load CatalogTreeModel
    tree_view_ = new QTreeView(this);
    tree_view_->setObjectName(QStringLiteral("ExplorerTree"));
    tree_view_->setHeaderHidden(true);
    tree_view_->setUniformRowHeights(true);
    tree_view_->setFrameShape(QFrame::NoFrame);
    tree_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_view_->setAccessibleName(QStringLiteral("Object Explorer Hierarchy"));
    tree_view_->installEventFilter(this);

    model_ = std::make_unique<CatalogTreeModel>(worker_pool_, this);
    tree_view_->setModel(model_.get());

    connect(tree_view_, &QTreeView::expanded, model_.get(), [this](const QModelIndex& idx) {
        model_->load_node_async(idx, false);
    });
    connect(tree_view_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex&, const QModelIndex&) {
        on_tree_selection_changed();
    });
    connect(tree_view_, &QTreeView::customContextMenuRequested,
            this, &ObjectExplorerPanel::open_context_menu);
    connect(model_.get(), &CatalogTreeModel::loading_state_changed, this, [this](bool loading) {
        set_loading_state(loading);
    });
    connect(model_.get(), &CatalogTreeModel::status_message, this, &ObjectExplorerPanel::status_message);

    root_layout->addWidget(tree_view_, 1);
}

bool ObjectExplorerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_view_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            const QModelIndex idx = tree_view_->currentIndex();
            if (idx.isValid()) {
                model_->cancel_node_load(idx);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QToolButton* ObjectExplorerPanel::toolbar_button(const std::string& id) const {
    if (const auto it = button_map_.find(id); it != button_map_.end()) {
        return it->second;
    }
    return nullptr;
}

bool ObjectExplorerPanel::is_toolbar_button_visible(const std::string& id) const {
    if (QToolButton* btn = toolbar_button(id)) {
        return !btn->isHidden();
    }
    return false;
}

bool ObjectExplorerPanel::is_connected() const noexcept {
    return model_ != nullptr && model_->has_servers();
}

std::optional<drivers::ConnectionConfig> ObjectExplorerPanel::selected_connection() const {
    if (model_ == nullptr || tree_view_ == nullptr) {
        return std::nullopt;
    }
    return model_->connection_for_index(tree_view_->currentIndex());
}

std::optional<drivers::ConnectionConfig> ObjectExplorerPanel::primary_connection() const {
    return model_ != nullptr ? model_->primary_connection() : std::nullopt;
}

void ObjectExplorerPanel::set_connected_server(
    const drivers::ConnectionConfig& config,
    const drivers::SessionInfo& info) {
    if (!model_) {
        return;
    }
    const QModelIndex root_idx = model_->add_connected_server(config, info, true);
    tree_view_->expand(root_idx);
    tree_view_->setCurrentIndex(root_idx);
    set_connected_state(true);
}

void ObjectExplorerPanel::disconnect_all() {
    if (model_) {
        model_->disconnect_all();
    }
    set_connected_state(false);
}

void ObjectExplorerPanel::set_connected_state(bool connected) {
    if (QToolButton* btn = toolbar_button("Disconnect")) {
        btn->setEnabled(connected);
    }
    if (!connected) {
        set_filter_enabled(false);
        set_loading_state(false);
    }
}

void ObjectExplorerPanel::set_loading_state(bool loading) {
    if (QToolButton* btn = toolbar_button("Stop")) {
        btn->setEnabled(loading);
        btn->setIcon(get_icon(loading ? IconId::Stop : IconId::StopIdle));
    }
}

void ObjectExplorerPanel::set_filter_enabled(bool enabled) {
    if (QToolButton* btn = toolbar_button("Filter")) {
        btn->setEnabled(enabled);
    }
}

void ObjectExplorerPanel::focus_tree() {
    if (tree_view_ != nullptr) {
        tree_view_->setFocus(Qt::ShortcutFocusReason);
    }
}

void ObjectExplorerPanel::on_tree_selection_changed() {
    const CatalogTreeNode* node = model_->node_from_index(tree_view_->currentIndex());
    set_filter_enabled(node != nullptr && node->data.can_filter());
}

void ObjectExplorerPanel::refresh_selected() {
    if (!model_ || !model_->has_servers()) {
        return;
    }
    QModelIndex idx = tree_view_->currentIndex();
    if (!idx.isValid()) {
        idx = model_->index(0, 0);
    }
    const CatalogTreeNode* node = model_->node_from_index(idx);
    if (node != nullptr && !node->data.can_expand() && idx.parent().isValid()) {
        idx = idx.parent();
    }
    model_->load_node_async(idx, true);
}

void ObjectExplorerPanel::stop_selected_load() {
    if (!model_) {
        return;
    }
    const QModelIndex idx = tree_view_->currentIndex();
    if (idx.isValid()) {
        model_->cancel_node_load(idx);
    } else {
        model_->cancel_all_loads();
    }
}

void ObjectExplorerPanel::open_filter_for_selected() {
    apply_filter_dialog(tree_view_->currentIndex());
}

void ObjectExplorerPanel::apply_filter_dialog(const QModelIndex& idx) {
    if (!model_) {
        return;
    }
    const CatalogTreeNode* node = model_->node_from_index(idx);
    if (node == nullptr || !node->data.can_filter()) {
        return;
    }

    const QString server = model_->server_display_for_index(idx);
    const QString db = (node->data.kind == drivers::CatalogKind::Roles)
        ? QString{}
        : QString::fromStdString(node->data.database);
    FilterSettingsDialog dlg(server, db, node->filter, node->data.can_filter_by_owner(), this);
    if (dlg.exec() == QDialog::Accepted) {
        model_->apply_filter(idx, dlg.filter_result());
        tree_view_->expand(idx);
    }
}

void ObjectExplorerPanel::open_context_menu(const QPoint& viewport_pos) {
    const QModelIndex idx = tree_view_->indexAt(viewport_pos);
    if (idx.isValid()) {
        tree_view_->setCurrentIndex(idx);
    }
    const CatalogTreeNode* node = model_->node_from_index(idx);

    const uint32_t select_top =
        static_cast<uint32_t>(core::load_environment_options(core::environment_options_path()).normalized_select_top());

    ExplorerContextCallbacks cb;
    cb.on_connect = [this]() {
        emit connect_requested();
    };
    cb.on_disconnect_server = [this](const QModelIndex& server_idx) {
        model_->disconnect_server(server_idx);
        const bool still_connected = model_->has_servers();
        set_connected_state(still_connected);
        if (!still_connected) {
            emit disconnect_requested();
        }
    };
    cb.on_new_query = [this](const QModelIndex& node_idx) {
        if (const auto cfg = model_->connection_for_index(node_idx); cfg.has_value()) {
            emit new_query_requested(*cfg);
        }
    };
    cb.on_select_top = [this](const QModelIndex& node_idx, uint32_t top_n) {
        const CatalogTreeNode* n = model_->node_from_index(node_idx);
        const auto cfg = model_->connection_for_index(node_idx);
        if (n != nullptr && cfg.has_value()) {
            emit select_top_requested(
                *cfg,
                QString::fromStdString(n->data.database),
                QString::fromStdString(n->data.schema),
                QString::fromStdString(n->data.name),
                top_n);
        }
    };
    cb.on_script_select = [this](const QModelIndex& node_idx, bool to_query) {
        const CatalogTreeNode* n = model_->node_from_index(node_idx);
        const auto cfg = model_->connection_for_index(node_idx);
        if (n != nullptr && cfg.has_value()) {
            emit script_select_requested(
                *cfg,
                QString::fromStdString(n->data.database),
                QString::fromStdString(n->data.schema),
                QString::fromStdString(n->data.name),
                to_query);
        }
    };
    cb.on_filter_settings = [this](const QModelIndex& node_idx) {
        apply_filter_dialog(node_idx);
    };
    cb.on_remove_filter = [this](const QModelIndex& node_idx) {
        model_->apply_filter(node_idx, drivers::CatalogFilter{});
    };
    cb.on_refresh = [this](const QModelIndex& node_idx) {
        model_->load_node_async(node_idx, true);
    };

    QMenu* menu = build_explorer_context_menu(node, idx, select_top, cb, this);
    menu->popup(tree_view_->viewport()->mapToGlobal(viewport_pos));
}

void ObjectExplorerPanel::load_toolbar_preferences() {
    const std::vector<std::string> hidden = capybaradb::core::load_explorer_hidden_toolbar_ids(prefs_path_);
    const std::unordered_set<std::string> hidden_set(hidden.begin(), hidden.end());
    for (const auto& entry : buttons_) {
        entry.button->setVisible(!hidden_set.contains(entry.id));
    }
}

void ObjectExplorerPanel::save_toolbar_preferences() {
    std::vector<std::string> hidden;
    for (const auto& entry : buttons_) {
        if (entry.button->isHidden()) {
            hidden.push_back(entry.id);
        }
    }
    if (!capybaradb::core::save_explorer_hidden_toolbar_ids(prefs_path_, hidden)) {
        emit status_message(QStringLiteral("The toolbar choice could not be saved."));
    }
}

void ObjectExplorerPanel::open_toolbar_options_menu() {
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* submenu = menu->addMenu(QStringLiteral("Add or Remove Buttons"));
    std::vector<QAction*> toggle_actions;
    for (const auto& entry : buttons_) {
        QAction* act = submenu->addAction(entry.label);
        act->setCheckable(true);
        act->setChecked(!entry.button->isHidden());
        if (!entry.gesture.isEmpty()) {
            act->setShortcut(QKeySequence(entry.gesture));
        }
        if (entry.id == "ConnectExplorer") {
            act->setIcon(get_icon(IconId::Plug));
        } else if (entry.id == "Disconnect") {
            act->setIcon(get_icon(IconId::PlugOff));
        } else if (entry.id == "Stop") {
            act->setIcon(get_icon(IconId::Stop));
        } else if (entry.id == "Filter") {
            act->setIcon(get_icon(IconId::Filter));
        } else if (entry.id == "Refresh") {
            act->setIcon(get_icon(IconId::Refresh));
        }
        QToolButton* target = entry.button;
        connect(act, &QAction::toggled, this, [this, target](bool checked) {
            target->setVisible(checked);
            save_toolbar_preferences();
        });
        toggle_actions.push_back(act);
    }

    submenu->addSeparator();
    QAction* reset_act = submenu->addAction(QStringLiteral("Reset Toolbar"));
    connect(reset_act, &QAction::triggered, this, [this, toggle_actions]() {
        for (const auto& entry : buttons_) {
            entry.button->setVisible(true);
        }
        for (QAction* act : toggle_actions) {
            act->setChecked(true);
        }
        save_toolbar_preferences();
    });

    const QPoint below = options_button_->mapToGlobal(QPoint(0, options_button_->height()));
    menu->popup(below);
}

} // namespace capybaradb::ui
