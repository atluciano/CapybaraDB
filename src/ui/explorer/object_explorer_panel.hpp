#pragma once

#include "core/config/environment_options.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "drivers/common/db_types.hpp"
#include "ui/explorer/catalog_tree_model.hpp"

#include <QWidget>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QModelIndex;
class QToolButton;
class QTreeView;

namespace capybaradb::ui {

class ObjectExplorerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectExplorerPanel(
        std::filesystem::path toolbar_prefs_path = {},
        core::CatalogWorkerPool* worker_pool = nullptr,
        QWidget* parent = nullptr);
    ~ObjectExplorerPanel() override;

    [[nodiscard]] QTreeView* tree_view() const noexcept { return tree_view_; }
    [[nodiscard]] CatalogTreeModel* catalog_model() const noexcept { return model_.get(); }
    [[nodiscard]] QToolButton* toolbar_button(const std::string& id) const;
    [[nodiscard]] bool is_toolbar_button_visible(const std::string& id) const;

    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] std::optional<drivers::ConnectionConfig> selected_connection() const;
    [[nodiscard]] std::optional<drivers::ConnectionConfig> primary_connection() const;

    void set_connected_server(const drivers::ConnectionConfig& config, const drivers::SessionInfo& info);
    void disconnect_all();

    void set_connected_state(bool connected);
    void set_loading_state(bool loading);
    void set_filter_enabled(bool enabled);

public slots:
    void focus_tree();
    void refresh_selected();
    void stop_selected_load();
    void open_filter_for_selected();

signals:
    void connect_requested();
    void disconnect_requested();
    void stop_requested();
    void filter_requested();
    void refresh_requested();
    void status_message(const QString& message);

    void new_query_requested(const capybaradb::drivers::ConnectionConfig& config);
    void select_top_requested(
        const capybaradb::drivers::ConnectionConfig& config,
        const QString& database,
        const QString& schema,
        const QString& table,
        uint32_t top_n);
    void script_select_requested(
        const capybaradb::drivers::ConnectionConfig& config,
        const QString& database,
        const QString& schema,
        const QString& table,
        bool to_new_query);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ToolButtonEntry {
        std::string  id;
        QString      label;
        QString      gesture;
        QToolButton* button = nullptr;
    };

    void build_ui();
    void load_toolbar_preferences();
    void save_toolbar_preferences();
    void open_toolbar_options_menu();
    void open_context_menu(const QPoint& viewport_pos);
    void on_tree_selection_changed();
    void apply_filter_dialog(const QModelIndex& idx);

    std::filesystem::path                        prefs_path_;
    core::CatalogWorkerPool*                     worker_pool_ = nullptr;
    std::vector<ToolButtonEntry>                 buttons_;
    std::unordered_map<std::string, QToolButton*> button_map_;
    QToolButton*                                 options_button_ = nullptr;
    QTreeView*                                   tree_view_      = nullptr;
    std::unique_ptr<CatalogTreeModel>            model_;
};

} // namespace capybaradb::ui
