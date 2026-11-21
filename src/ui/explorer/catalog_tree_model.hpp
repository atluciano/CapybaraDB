#pragma once

#include "core/threading/cancel_token.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "drivers/common/catalog_provider.hpp"
#include "drivers/common/db_driver.hpp"
#include "drivers/common/db_types.hpp"

#include <QAbstractItemModel>
#include <QIcon>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace capybaradb::ui {

struct ServerRootContext {
    drivers::ConnectionConfig config;
    drivers::SessionInfo      info;
};

struct CatalogTreeNode {
    uint64_t                                      node_id = 0;
    CatalogTreeNode*                              parent = nullptr;
    int                                           row_in_parent = 0;

    std::unique_ptr<ServerRootContext>            server_root;
    drivers::CatalogNodeData                      data;
    drivers::CatalogFilter                        filter;

    bool                                          loaded       = false;
    bool                                          busy         = false;
    bool                                          was_canceled = false;
    std::string                                   error_text;
    uint64_t                                      load_generation = 0;
    std::shared_ptr<core::CancelToken>            cancel_token;

    std::vector<std::unique_ptr<CatalogTreeNode>> all_children;
    std::unique_ptr<CatalogTreeNode>              placeholder_child;
    std::vector<CatalogTreeNode*>                 visible_children;

    [[nodiscard]] bool is_server() const noexcept { return data.kind == drivers::CatalogKind::Server; }
    [[nodiscard]] CatalogTreeNode* load_target() noexcept {
        if (data.can_expand()) {
            return this;
        }
        return parent;
    }
};

class CatalogTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    using DriverFactory   = std::function<std::unique_ptr<drivers::DbDriver>(drivers::EngineKind)>;
    using ProviderFactory = std::function<std::unique_ptr<drivers::CatalogProvider>(drivers::EngineKind)>;

    explicit CatalogTreeModel(
        core::CatalogWorkerPool* worker_pool = nullptr,
        QObject* parent = nullptr);
    ~CatalogTreeModel() override;

    // QAbstractItemModel overrides
    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;

    // Server & catalog lifecycle
    [[nodiscard]] QModelIndex add_connected_server(
        const drivers::ConnectionConfig& config,
        const drivers::SessionInfo& info,
        bool replace_same_host = true);
    void disconnect_all();
    void disconnect_server(const QModelIndex& any_index);

    void load_node_async(const QModelIndex& index, bool refresh = false);
    void cancel_node_load(const QModelIndex& index);
    void cancel_all_loads();
    void apply_filter(const QModelIndex& index, const drivers::CatalogFilter& filter);

    [[nodiscard]] bool has_servers() const noexcept { return !roots_.empty(); }
    [[nodiscard]] bool any_loading() const noexcept;
    [[nodiscard]] const CatalogTreeNode* node_from_index(const QModelIndex& index) const noexcept;
    [[nodiscard]] CatalogTreeNode* node_from_index(const QModelIndex& index) noexcept;
    [[nodiscard]] QModelIndex index_for_node(const CatalogTreeNode* node) const;

    [[nodiscard]] std::optional<drivers::ConnectionConfig> connection_for_index(const QModelIndex& index) const;
    [[nodiscard]] std::optional<drivers::ConnectionConfig> primary_connection() const;
    [[nodiscard]] QString server_display_for_index(const QModelIndex& index) const;

    // Dependency injection hooks for unit/UI tests
    void set_factories(DriverFactory driver_factory, ProviderFactory provider_factory);

signals:
    void loading_state_changed(bool any_loading);
    void status_message(const QString& text);
    void server_load_failed(const QString& message);
    void node_load_completed(const QModelIndex& index);

private:
    CatalogTreeNode* allocate_node(CatalogTreeNode* parent);
    void unregister_subtree(const CatalogTreeNode* node);
    void populate_visible_children(CatalogTreeNode* parent);
    void set_children_from_entries(CatalogTreeNode* parent, const std::vector<drivers::CatalogNodeData>& entries);
    void set_placeholder_child(CatalogTreeNode* parent, const std::string& label, bool emit_signals = true);
    void notify_node_changed(CatalogTreeNode* node);
    [[nodiscard]] QString display_text_for(const CatalogTreeNode* node) const;
    [[nodiscard]] QString tooltip_for(const CatalogTreeNode* node) const;
    [[nodiscard]] QIcon icon_for(const CatalogTreeNode* node) const;
    [[nodiscard]] const CatalogTreeNode* root_of(const CatalogTreeNode* node) const noexcept;

    core::CatalogWorkerPool*                  worker_pool_ = nullptr;
    std::unique_ptr<core::CatalogWorkerPool>  owned_pool_;
    DriverFactory                             driver_factory_;
    ProviderFactory                           provider_factory_;

    uint64_t                                      next_node_id_ = 1;
    std::vector<std::unique_ptr<CatalogTreeNode>> roots_;
    std::unordered_map<uint64_t, CatalogTreeNode*> node_by_id_;
};

} // namespace capybaradb::ui
