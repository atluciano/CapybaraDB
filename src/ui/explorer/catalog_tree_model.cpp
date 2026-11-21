#include "ui/explorer/catalog_tree_model.hpp"

#include "drivers/common/driver_registry.hpp"
#include "ui/shell/icon_catalog.hpp"

#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <unordered_map>

namespace capybaradb::ui {
namespace {

QString short_server_version(const std::string& raw, drivers::EngineKind engine) {
    if (raw.empty()) {
        if (engine == drivers::EngineKind::Sqlite) {
            return QStringLiteral("SQLite");
        }
        return engine == drivers::EngineKind::SqlServer
            ? QStringLiteral("SQL Server")
            : QStringLiteral("PostgreSQL");
    }
    const QString full = QString::fromStdString(raw).trimmed();
    // PostgreSQL version(): "PostgreSQL 17.2 on x86_64-..." -> "PostgreSQL 17.2"
    if (engine == drivers::EngineKind::PostgreSql) {
        const QStringList parts = full.split(u' ', Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            return parts[0] + u' ' + parts[1];
        }
    }
    // SQL Server @@VERSION: take first line up to '-' or newline
    const int nl = full.indexOf(u'\n');
    QString first_line = (nl > 0) ? full.left(nl).trimmed() : full;
    if (first_line.size() > 36) {
        first_line = first_line.left(33) + QStringLiteral("…");
    }
    return first_line;
}

std::string node_identity_key(const drivers::CatalogNodeData& d) {
    return std::to_string(static_cast<int>(d.kind)) + "|" +
           std::to_string(d.oid) + "|" +
           d.database + "|" + d.schema + "|" + d.name;
}

} // namespace

CatalogTreeModel::CatalogTreeModel(
    core::CatalogWorkerPool* worker_pool,
    QObject* parent)
    : QAbstractItemModel(parent)
    , worker_pool_(worker_pool) {
    driver_factory_ = [](drivers::EngineKind engine) {
        return drivers::create_db_driver(engine);
    };
    provider_factory_ = [](drivers::EngineKind engine) {
        return drivers::create_catalog_provider(engine);
    };
}

CatalogTreeModel::~CatalogTreeModel() {
    cancel_all_loads();
    owned_pool_.reset();
    node_by_id_.clear();
    roots_.clear();
}

void CatalogTreeModel::set_factories(DriverFactory driver_factory, ProviderFactory provider_factory) {
    if (driver_factory) {
        driver_factory_ = std::move(driver_factory);
    }
    if (provider_factory) {
        provider_factory_ = std::move(provider_factory);
    }
}

CatalogTreeNode* CatalogTreeModel::allocate_node(CatalogTreeNode* parent) {
    auto node = std::make_unique<CatalogTreeNode>();
    node->node_id = next_node_id_++;
    node->parent  = parent;
    CatalogTreeNode* raw = node.get();
    node_by_id_.emplace(raw->node_id, raw);

    if (parent != nullptr) {
        parent->all_children.push_back(std::move(node));
    } else {
        raw->row_in_parent = static_cast<int>(roots_.size());
        roots_.push_back(std::move(node));
    }
    return raw;
}

void CatalogTreeModel::unregister_subtree(const CatalogTreeNode* node) {
    if (node == nullptr) {
        return;
    }
    if (node->cancel_token) {
        node->cancel_token->cancel();
    }
    if (node->placeholder_child) {
        node_by_id_.erase(node->placeholder_child->node_id);
    }
    for (const auto& ch : node->all_children) {
        unregister_subtree(ch.get());
    }
    node_by_id_.erase(node->node_id);
}

QModelIndex CatalogTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (column != 0 || row < 0) {
        return {};
    }
    if (!parent.isValid()) {
        if (row >= static_cast<int>(roots_.size())) {
            return {};
        }
        return createIndex(row, 0, roots_[static_cast<size_t>(row)].get());
    }
    const auto* pnode = node_from_index(parent);
    if (pnode == nullptr || row >= static_cast<int>(pnode->visible_children.size())) {
        return {};
    }
    return createIndex(row, 0, pnode->visible_children[static_cast<size_t>(row)]);
}

QModelIndex CatalogTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const auto* node = node_from_index(child);
    if (node == nullptr || node->parent == nullptr) {
        return {};
    }
    return createIndex(node->parent->row_in_parent, 0, node->parent);
}

int CatalogTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    const auto* node = node_from_index(parent);
    return node != nullptr ? static_cast<int>(node->visible_children.size()) : 0;
}

int CatalogTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

bool CatalogTreeModel::hasChildren(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return !roots_.empty();
    }
    const auto* node = node_from_index(parent);
    if (node == nullptr) {
        return false;
    }
    return !node->visible_children.empty() || node->data.can_expand();
}

Qt::ItemFlags CatalogTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant CatalogTreeModel::data(const QModelIndex& index, int role) const {
    const auto* node = node_from_index(index);
    if (node == nullptr) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return display_text_for(node);
    case Qt::DecorationRole: {
        const QIcon ic = icon_for(node);
        return ic.isNull() ? QVariant{} : QVariant{ic};
    }
    case Qt::ToolTipRole: {
        const QString tip = tooltip_for(node);
        return tip.isEmpty() ? QVariant{} : QVariant{tip};
    }
    default:
        return {};
    }
}

const CatalogTreeNode* CatalogTreeModel::node_from_index(const QModelIndex& index) const noexcept {
    if (!index.isValid()) {
        return nullptr;
    }
    return static_cast<const CatalogTreeNode*>(index.internalPointer());
}

CatalogTreeNode* CatalogTreeModel::node_from_index(const QModelIndex& index) noexcept {
    if (!index.isValid()) {
        return nullptr;
    }
    return static_cast<CatalogTreeNode*>(index.internalPointer());
}

QModelIndex CatalogTreeModel::index_for_node(const CatalogTreeNode* node) const {
    if (node == nullptr) {
        return {};
    }
    return createIndex(node->row_in_parent, 0, const_cast<CatalogTreeNode*>(node));
}

const CatalogTreeNode* CatalogTreeModel::root_of(const CatalogTreeNode* node) const noexcept {
    while (node != nullptr && node->parent != nullptr) {
        node = node->parent;
    }
    return node;
}

std::optional<drivers::ConnectionConfig> CatalogTreeModel::connection_for_index(const QModelIndex& index) const {
    const CatalogTreeNode* node = node_from_index(index);
    const CatalogTreeNode* root = root_of(node);
    if (root == nullptr && !roots_.empty()) {
        root = roots_.front().get();
    }
    if (root == nullptr || !root->server_root) {
        return std::nullopt;
    }
    drivers::ConnectionConfig cfg = root->server_root->config;
    if (node != nullptr && !node->data.database.empty()) {
        cfg.database = node->data.database;
    }
    return cfg;
}

std::optional<drivers::ConnectionConfig> CatalogTreeModel::primary_connection() const {
    if (roots_.empty() || !roots_.front()->server_root) {
        return std::nullopt;
    }
    return roots_.front()->server_root->config;
}

QString CatalogTreeModel::server_display_for_index(const QModelIndex& index) const {
    const CatalogTreeNode* root = root_of(node_from_index(index));
    if (root == nullptr && !roots_.empty()) {
        root = roots_.front().get();
    }
    if (root == nullptr || !root->server_root) {
        return {};
    }
    return QString::fromStdString(root->server_root->config.host + ":" + std::to_string(root->server_root->config.port));
}

QString CatalogTreeModel::display_text_for(const CatalogTreeNode* node) const {
    if (node == nullptr) {
        return {};
    }
    QString text = QString::fromStdString(node->data.name);
    if (node->filter.is_active()) {
        text += QStringLiteral(" (filtered)");
    }
    if (node->busy) {
        text += QStringLiteral(" (loading)");
    } else if (node->was_canceled) {
        text += QStringLiteral(" (load canceled)");
    } else if (!node->error_text.empty()) {
        text += QStringLiteral(" (load failed)");
    }
    return text;
}

QString CatalogTreeModel::tooltip_for(const CatalogTreeNode* node) const {
    if (node == nullptr) {
        return {};
    }
    if (!node->error_text.empty()) {
        return QString::fromStdString(node->error_text);
    }
    if (node->is_server() && node->server_root && !node->server_root->info.server_version.empty()) {
        return QString::fromStdString(node->server_root->info.server_version);
    }
    return {};
}

QIcon CatalogTreeModel::icon_for(const CatalogTreeNode* node) const {
    using drivers::CatalogKind;
    if (node == nullptr || node->data.kind == CatalogKind::Placeholder) {
        return {};
    }
    switch (node->data.kind) {
    case CatalogKind::Server: {
        const auto engine = node->server_root ? node->server_root->config.engine : drivers::EngineKind::PostgreSql;
        if (engine == drivers::EngineKind::Sqlite) {
            return get_icon(IconId::Database);
        }
        return get_icon(
            engine == drivers::EngineKind::SqlServer
                ? IconId::EngineMssql
                : IconId::EnginePostgres);
    }
    case CatalogKind::Database:
        return get_icon(IconId::Database);
    case CatalogKind::Schema:
        return get_icon(IconId::Schema);
    case CatalogKind::Relation:
        if (node->data.relation_type == 'v') return get_icon(IconId::View);
        if (node->data.relation_type == 'm') return get_icon(IconId::MaterializedView);
        if (node->data.relation_type == 'S') return get_icon(IconId::Sequence);
        if (node->data.relation_type == 'p') return get_icon(IconId::Partition);
        return get_icon(IconId::Table);
    case CatalogKind::Routine:
    case CatalogKind::Trigger:
        return get_icon(IconId::Routine);
    case CatalogKind::Column:
        return get_icon(IconId::Column);
    case CatalogKind::Index:
    case CatalogKind::Key:
        return get_icon(IconId::Index);
    case CatalogKind::Constraint:
        return get_icon(IconId::Domain);
    case CatalogKind::Role:
        return get_icon(IconId::Role);
    case CatalogKind::DataType:
        return get_icon(node->data.data_type_kind == 'd' ? IconId::Domain : IconId::Enum);
    case CatalogKind::DatabaseDiagram:
        return get_icon(IconId::Diagram);
    default:
        return get_icon(IconId::Folder);
    }
}

void CatalogTreeModel::notify_node_changed(CatalogTreeNode* node) {
    if (node == nullptr) {
        return;
    }
    const QModelIndex idx = index_for_node(node);
    if (idx.isValid()) {
        emit dataChanged(idx, idx);
    }
}

void CatalogTreeModel::set_placeholder_child(
    CatalogTreeNode* parent,
    const std::string& label,
    bool emit_signals) {
    if (parent == nullptr) {
        return;
    }
    const QModelIndex parent_idx = emit_signals ? index_for_node(parent) : QModelIndex{};

    if (!parent->visible_children.empty()) {
        if (emit_signals) {
            beginRemoveRows(parent_idx, 0, static_cast<int>(parent->visible_children.size()) - 1);
        }
        parent->visible_children.clear();
        if (emit_signals) {
            endRemoveRows();
        }
    }

    if (parent->placeholder_child) {
        node_by_id_.erase(parent->placeholder_child->node_id);
        parent->placeholder_child.reset();
    }

    auto ph = std::make_unique<CatalogTreeNode>();
    ph->node_id       = next_node_id_++;
    ph->parent        = parent;
    ph->row_in_parent = 0;
    ph->data.kind     = drivers::CatalogKind::Placeholder;
    ph->data.name     = label;
    ph->data.database = parent->data.database;
    ph->data.schema   = parent->data.schema;
    node_by_id_.emplace(ph->node_id, ph.get());
    parent->placeholder_child = std::move(ph);

    if (emit_signals) {
        beginInsertRows(parent_idx, 0, 0);
    }
    parent->visible_children.push_back(parent->placeholder_child.get());
    if (emit_signals) {
        endInsertRows();
    }
}

void CatalogTreeModel::populate_visible_children(CatalogTreeNode* parent) {
    if (parent == nullptr) {
        return;
    }
    const QModelIndex parent_idx = index_for_node(parent);

    if (!parent->visible_children.empty()) {
        beginRemoveRows(parent_idx, 0, static_cast<int>(parent->visible_children.size()) - 1);
        parent->visible_children.clear();
        endRemoveRows();
    }

    std::vector<CatalogTreeNode*> next_vis;
    for (const auto& ch : parent->all_children) {
        if (parent->filter.matches(ch->data.filter_name(), ch->data.owner)) {
            next_vis.push_back(ch.get());
        }
    }

    if (next_vis.empty()) {
        const char* msg = parent->all_children.empty() ? "(empty)" : "(no matching items)";
        set_placeholder_child(parent, msg);
        return;
    }

    if (parent->placeholder_child) {
        node_by_id_.erase(parent->placeholder_child->node_id);
        parent->placeholder_child.reset();
    }

    beginInsertRows(parent_idx, 0, static_cast<int>(next_vis.size()) - 1);
    for (int i = 0; i < static_cast<int>(next_vis.size()); ++i) {
        next_vis[static_cast<size_t>(i)]->row_in_parent = i;
    }
    parent->visible_children = std::move(next_vis);
    endInsertRows();
}

void CatalogTreeModel::set_children_from_entries(
    CatalogTreeNode* parent,
    const std::vector<drivers::CatalogNodeData>& entries) {
    if (parent == nullptr) {
        return;
    }

    // Clear visible_children cleanly before replacing all_children to keep model indices valid.
    const QModelIndex parent_idx = index_for_node(parent);
    if (!parent->visible_children.empty()) {
        beginRemoveRows(parent_idx, 0, static_cast<int>(parent->visible_children.size()) - 1);
        parent->visible_children.clear();
        endRemoveRows();
    }

    std::unordered_map<std::string, std::unique_ptr<CatalogTreeNode>> old_map;
    for (auto& old : parent->all_children) {
        old_map.emplace(node_identity_key(old->data), std::move(old));
    }
    parent->all_children.clear();

    for (const drivers::CatalogNodeData& entry : entries) {
        const std::string key = node_identity_key(entry);
        if (const auto it = old_map.find(key); it != old_map.end() && it->second != nullptr) {
            it->second->data   = entry;
            it->second->parent = parent;
            parent->all_children.push_back(std::move(it->second));
            old_map.erase(it);
        } else {
            auto node = std::make_unique<CatalogTreeNode>();
            node->node_id = next_node_id_++;
            node->parent  = parent;
            node->data    = entry;
            node_by_id_.emplace(node->node_id, node.get());
            if (entry.can_expand()) {
                set_placeholder_child(node.get(), "Expand to load…", false);
            }
            parent->all_children.push_back(std::move(node));
        }
    }

    for (const auto& [_, stale] : old_map) {
        if (stale) {
            unregister_subtree(stale.get());
        }
    }

    parent->loaded = true;
    populate_visible_children(parent);
}

QModelIndex CatalogTreeModel::add_connected_server(
    const drivers::ConnectionConfig& config,
    const drivers::SessionInfo& info,
    bool replace_same_host) {
    if (replace_same_host) {
        for (size_t i = 0; i < roots_.size(); ++i) {
            if (!roots_[i]->server_root) continue;
            const auto& r = roots_[i]->server_root->config;
            if (r.engine == config.engine && r.host == config.host && r.port == config.port) {
                beginRemoveRows({}, static_cast<int>(i), static_cast<int>(i));
                unregister_subtree(roots_[i].get());
                roots_.erase(roots_.begin() + static_cast<ptrdiff_t>(i));
                for (size_t j = 0; j < roots_.size(); ++j) {
                    roots_[j]->row_in_parent = static_cast<int>(j);
                }
                endRemoveRows();
                break;
            }
        }
    }

    const int new_row = static_cast<int>(roots_.size());
    beginInsertRows({}, new_row, new_row);
    auto node = std::make_unique<CatalogTreeNode>();
    node->node_id       = next_node_id_++;
    node->parent        = nullptr;
    node->row_in_parent = new_row;
    node->server_root   = std::make_unique<ServerRootContext>(ServerRootContext{config, info});
    node->data.kind     = drivers::CatalogKind::Server;
    node->data.database = config.database;
    node->data.name     = config.display_server() + " — " +
        short_server_version(info.server_version, config.engine).toStdString();

    CatalogTreeNode* raw = node.get();
    node_by_id_.emplace(raw->node_id, raw);
    roots_.push_back(std::move(node));
    endInsertRows();

    set_placeholder_child(raw, "Expand to load…");
    const QModelIndex root_idx = index_for_node(raw);
    load_node_async(root_idx, true);
    return root_idx;
}

void CatalogTreeModel::disconnect_all() {
    if (roots_.empty()) {
        return;
    }
    beginResetModel();
    for (const auto& r : roots_) {
        unregister_subtree(r.get());
    }
    roots_.clear();
    node_by_id_.clear();
    endResetModel();
    emit loading_state_changed(false);
    emit status_message(QStringLiteral("Object Explorer disconnected."));
}

void CatalogTreeModel::disconnect_server(const QModelIndex& any_index) {
    const CatalogTreeNode* root = root_of(node_from_index(any_index));
    if (root == nullptr) {
        return;
    }
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (roots_[i].get() == root) {
            beginRemoveRows({}, static_cast<int>(i), static_cast<int>(i));
            unregister_subtree(roots_[i].get());
            roots_.erase(roots_.begin() + static_cast<ptrdiff_t>(i));
            for (size_t j = 0; j < roots_.size(); ++j) {
                roots_[j]->row_in_parent = static_cast<int>(j);
            }
            endRemoveRows();
            break;
        }
    }
    emit loading_state_changed(any_loading());
}

bool CatalogTreeModel::any_loading() const noexcept {
    for (const auto& [_, node] : node_by_id_) {
        if (node != nullptr && node->busy) {
            return true;
        }
    }
    return false;
}

void CatalogTreeModel::load_node_async(const QModelIndex& index, bool refresh) {
    CatalogTreeNode* node = node_from_index(index);
    if (node == nullptr || !node->data.can_expand()) {
        return;
    }
    if (node->busy || (node->loaded && !refresh && node->error_text.empty() && !node->was_canceled)) {
        return;
    }

    if (worker_pool_ == nullptr && !owned_pool_) {
        owned_pool_ = std::make_unique<core::CatalogWorkerPool>(2);
    }
    core::CatalogWorkerPool* pool = worker_pool_ ? worker_pool_ : owned_pool_.get();

    const CatalogTreeNode* root = root_of(node);
    const drivers::ConnectionConfig base_cfg =
        (root != nullptr && root->server_root) ? root->server_root->config : drivers::ConnectionConfig{};
    const drivers::CatalogNodeData target_data = node->data;
    const uint64_t node_id = node->node_id;
    const uint64_t gen     = ++node->load_generation;

    node->cancel_token = std::make_shared<core::CancelToken>();
    node->busy         = true;
    node->was_canceled = false;
    node->error_text.clear();
    notify_node_changed(node);
    emit loading_state_changed(true);

    QPointer<CatalogTreeModel> self(this);
    auto token    = node->cancel_token;
    auto factory  = provider_factory_;

    pool->submit([self, node_id, gen, base_cfg, target_data, token, factory]() {
        auto provider = factory(base_cfg.engine);
        auto res      = provider->list_children(base_cfg, target_data, *token);

        QMetaObject::invokeMethod(
            self,
            [self, node_id, gen, token, res = std::move(res)]() {
                if (!self) {
                    return;
                }
                const auto it = self->node_by_id_.find(node_id);
                if (it == self->node_by_id_.end() || it->second == nullptr) {
                    emit self->loading_state_changed(self->any_loading());
                    return;
                }
                CatalogTreeNode* target = it->second;
                if (target->load_generation != gen) {
                    return;
                }

                target->busy = false;
                target->cancel_token.reset();

                if (token->is_canceled()) {
                    target->was_canceled = true;
                } else if (res.has_value()) {
                    target->was_canceled = false;
                    target->error_text.clear();
                    self->set_children_from_entries(target, *res);
                    if (target->is_server()) {
                        emit self->status_message(
                            QStringLiteral("Object Explorer refreshed. Expand a database to load its objects."));
                    }
                    emit self->node_load_completed(self->index_for_node(target));
                } else if (res.error().is_canceled) {
                    target->was_canceled = true;
                } else {
                    target->error_text = res.error().format_short();
                    if (!target->loaded) {
                        self->set_placeholder_child(
                            target,
                            "Could not load: " + target->error_text + " (collapse and expand to retry)");
                    }
                    emit self->status_message(QString::fromStdString(target->error_text));
                    if (target->is_server()) {
                        emit self->server_load_failed(QString::fromStdString(target->error_text));
                    }
                }

                self->notify_node_changed(target);
                emit self->loading_state_changed(self->any_loading());
            },
            Qt::QueuedConnection);
    });
}

void CatalogTreeModel::cancel_node_load(const QModelIndex& index) {
    CatalogTreeNode* node = node_from_index(index);
    if (node == nullptr) {
        return;
    }
    if (CatalogTreeNode* target = node->load_target(); target != nullptr && target->busy && target->cancel_token) {
        target->was_canceled = true;
        target->cancel_token->cancel();
        notify_node_changed(target);
    }
}

void CatalogTreeModel::cancel_all_loads() {
    for (const auto& [_, node] : node_by_id_) {
        if (node != nullptr && node->cancel_token) {
            node->cancel_token->cancel();
        }
    }
}

void CatalogTreeModel::apply_filter(const QModelIndex& index, const drivers::CatalogFilter& filter) {
    CatalogTreeNode* node = node_from_index(index);
    if (node == nullptr || !node->data.can_filter()) {
        return;
    }
    node->filter = filter;
    if (node->loaded) {
        populate_visible_children(node);
    } else {
        load_node_async(index, false);
    }
    notify_node_changed(node);
    if (node->loaded) {
        const size_t total = node->all_children.size();
        const size_t shown = std::count_if(
            node->all_children.begin(),
            node->all_children.end(),
            [&](const auto& ch) {
                return node->filter.matches(ch->data.filter_name(), ch->data.owner);
            });
        emit status_message(QStringLiteral("%1 of %2 items.").arg(shown).arg(total));
    }
}

} // namespace capybaradb::ui
