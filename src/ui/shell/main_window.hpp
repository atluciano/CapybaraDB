#pragma once

#include "core/config/environment_options.hpp"
#include "core/config/query_history.hpp"
#include "core/plan/execution_plan.hpp"
#include "core/threading/cancel_token.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "drivers/common/db_types.hpp"

#include <QMainWindow>
#include <QMessageBox>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QLabel;
class QShowEvent;
class QStatusBar;
class QToolBar;

namespace ads {
class CDockAreaWidget;
class CDockManager;
class CDockWidget;
} // namespace ads

namespace capybaradb::ui {

class ExecutionPlanWidget;
class ObjectExplorerPanel;
class QueryTabWidget;
class RegisteredServersPanel;
class ShortcutRouter;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::filesystem::path state_dir = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] ads::CDockManager* dock_manager() const noexcept { return dock_manager_; }
    [[nodiscard]] ads::CDockWidget* explorer_dock() const noexcept { return explorer_dock_; }
    [[nodiscard]] ObjectExplorerPanel* object_explorer() const noexcept { return explorer_panel_; }
    [[nodiscard]] ads::CDockWidget* registered_servers_dock() const noexcept { return reg_servers_dock_; }
    [[nodiscard]] RegisteredServersPanel* registered_servers_panel() const noexcept { return reg_servers_panel_; }
    [[nodiscard]] ShortcutRouter* shortcut_router() const noexcept { return shortcut_router_; }

    [[nodiscard]] QToolBar* toolbar_band0() const noexcept { return toolbar_band0_; }
    [[nodiscard]] QToolBar* toolbar_band1() const noexcept { return toolbar_band1_; }
    [[nodiscard]] QComboBox* available_databases_combo() const noexcept { return db_combo_; }
    [[nodiscard]] QAction* action_execute() const noexcept { return act_execute_; }
    [[nodiscard]] QAction* action_cancel() const noexcept { return act_cancel_; }
    [[nodiscard]] QAction* action_parse() const noexcept { return act_parse_; }
    [[nodiscard]] QAction* action_estimated_plan() const noexcept { return act_est_plan_; }
    [[nodiscard]] QAction* action_open_plan() const noexcept { return act_open_plan_; }
    [[nodiscard]] QAction* action_find() const noexcept { return act_find_; }
    [[nodiscard]] QAction* action_replace() const noexcept { return act_replace_; }
    [[nodiscard]] QAction* action_go_to_line() const noexcept { return act_go_to_line_; }
    [[nodiscard]] QAction* action_comment() const noexcept { return act_comment_; }
    [[nodiscard]] QAction* action_uncomment() const noexcept { return act_uncomment_; }
    [[nodiscard]] QAction* action_results_text() const noexcept { return act_results_text_; }
    [[nodiscard]] QAction* action_results_grid() const noexcept { return act_results_grid_; }
    [[nodiscard]] QAction* action_results_file() const noexcept { return act_results_file_; }
    [[nodiscard]] QAction* action_query_options() const noexcept { return act_query_options_; }
    [[nodiscard]] QAction* action_options() const noexcept { return act_options_; }
    [[nodiscard]] QAction* action_registered_servers() const noexcept { return act_reg_servers_; }
    [[nodiscard]] QAction* action_query_history() const noexcept { return act_query_history_; }
    [[nodiscard]] QStatusBar* global_status_bar() const noexcept { return global_status_bar_; }
    [[nodiscard]] QLabel* global_status_label() const noexcept { return status_label_; }

    /// Test hooks to answer modal prompts without blocking in headless tests.
    void set_dirty_close_prompt(std::function<QMessageBox::StandardButton(QueryTabWidget*)> handler) {
        dirty_close_prompt_ = std::move(handler);
    }
    void set_open_tx_close_prompt(std::function<QMessageBox::StandardButton(bool aborted)> handler) {
        open_tx_close_prompt_ = std::move(handler);
    }
    void set_recovery_prompt(std::function<bool(int draft_count)> handler) {
        recovery_prompt_ = std::move(handler);
    }
    void set_open_plan_file_picker(std::function<QString()> picker) {
        open_plan_file_picker_ = std::move(picker);
    }

    [[nodiscard]] QueryTabWidget* active_query() const;
    [[nodiscard]] int query_tab_count() const noexcept { return static_cast<int>(query_docks_.size()); }
    [[nodiscard]] int plan_tab_count() const noexcept { return static_cast<int>(plan_docks_.size()); }
    [[nodiscard]] QString active_tab_title() const;
    [[nodiscard]] std::vector<ads::CDockWidget*> query_docks() const { return query_docks_; }
    [[nodiscard]] std::vector<ads::CDockWidget*> plan_docks() const { return plan_docks_; }
    [[nodiscard]] core::CatalogWorkerPool& worker_pool() noexcept { return worker_pool_; }
    void checkpoint_all_drafts_sync();

public slots:
    QueryTabWidget* new_query(const QString& explicit_title = {}, const QString& initial_text = {});
    QueryTabWidget* open_json_tab(const QString& pretty_json);
    ExecutionPlanWidget* open_plan_tab(
        std::vector<capybaradb::core::ExecutionPlanStatement> statements,
        const QString& explicit_title = {});
    QueryTabWidget* new_query_in_context();
    bool close_active_query();
    void show_object_explorer();
    void show_registered_servers();
    void show_query_history_dialog();
    void open_query_options_dialog();
    void open_environment_options_dialog();
    void open_execution_plan_file();
    bool check_draft_recovery();
    void reset_window_layout();
    void save_persistent_state();
    void restore_persistent_state();
    void prompt_connect_at_startup();
    void connect_with_dialog();
    void connect_explorer_with_config(const capybaradb::drivers::ConnectionConfig& cfg);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct QueryEntry {
        QueryTabWidget*  widget = nullptr;
        ads::CDockWidget* dock  = nullptr;
        QString          base_title;
    };

    void create_actions();
    void create_menu_bar();
    void create_tool_bars();
    void create_status_bar();
    void create_dock_manager();
    void connect_shortcuts();
    void refresh_commands();
    void apply_default_window_geometry();
    void restore_window_placement();
    void restore_dock_layout();
    void set_results_destination(capybaradb::core::ResultsDestination dest);
    void show_about_dialog();
    void update_tab_title(QueryTabWidget* query);
    void apply_explorer_splitter_width(int width_px);
    void update_workspace_area_style();
    void connect_active_query_with_dialog();
    void change_active_query_connection();
    void disconnect_active_query();
    void open_active_server_in_explorer();
    void populate_databases_popup();
    void on_database_activated(int index);
    void open_files_dialog();
    bool save_query_tab(QueryTabWidget* tab, bool save_as);
    bool confirm_close_tab(QueryTabWidget* tab);
    void record_query_history(const core::QueryHistoryCapture& capture);
    void checkpoint_query_draft(QueryTabWidget* tab, bool sync);
    void remove_query_draft(QueryTabWidget* tab);
    void open_history_entry_in_new_query(
        const QString& sql,
        const QString& engine,
        const QString& server,
        int port,
        const QString& database,
        const QString& user);
    void run_select_top(
        const drivers::ConnectionConfig& cfg,
        const QString& database,
        const QString& schema,
        const QString& table,
        uint32_t top_n);
    void run_script_select(
        const drivers::ConnectionConfig& cfg,
        const QString& database,
        const QString& schema,
        const QString& table,
        bool to_new_query);

    [[nodiscard]] ads::CDockAreaWidget* find_document_area() const;

    std::filesystem::path              state_dir_;
    int                                query_sequence_           = 0;
    int                                json_sequence_            = 0;
    int                                plan_sequence_            = 0;
    int                                preferred_explorer_width_ = capybaradb::core::EnvironmentDocument::kDefaultExplorerWidth;
    bool                               initial_show_done_        = false;
    core::CatalogWorkerPool            worker_pool_;
    std::shared_ptr<core::CancelToken> db_list_cancel_;
    uint64_t                           db_list_generation_       = 0;

    std::function<QMessageBox::StandardButton(QueryTabWidget*)> dirty_close_prompt_;
    std::function<QMessageBox::StandardButton(bool)>            open_tx_close_prompt_;
    std::function<bool(int)>                                    recovery_prompt_;
    std::function<QString()>                                    open_plan_file_picker_;

    ads::CDockManager*      dock_manager_      = nullptr;
    ads::CDockWidget*       workspace_dock_    = nullptr;
    ads::CDockWidget*       explorer_dock_     = nullptr;
    ObjectExplorerPanel*    explorer_panel_    = nullptr;
    ads::CDockWidget*       reg_servers_dock_  = nullptr;
    RegisteredServersPanel* reg_servers_panel_ = nullptr;
    ShortcutRouter*         shortcut_router_   = nullptr;

    std::vector<QueryEntry>        query_entries_;
    std::vector<ads::CDockWidget*> query_docks_;
    std::vector<ads::CDockWidget*> plan_docks_;

    // Toolbars & Status Bar
    QToolBar*   toolbar_band0_     = nullptr;
    QToolBar*   toolbar_band1_     = nullptr;
    QComboBox*  db_combo_          = nullptr;
    QStatusBar* global_status_bar_ = nullptr;
    QLabel*     status_label_      = nullptr;
    QLabel*     build_label_       = nullptr;

    // Actions
    QAction* act_new_query_        = nullptr;
    QAction* act_open_             = nullptr;
    QAction* act_open_plan_        = nullptr;
    QAction* act_save_             = nullptr;
    QAction* act_save_as_          = nullptr;
    QAction* act_close_tab_        = nullptr;
    QAction* act_exit_             = nullptr;

    QAction* act_undo_             = nullptr;
    QAction* act_redo_             = nullptr;
    QAction* act_cut_              = nullptr;
    QAction* act_copy_             = nullptr;
    QAction* act_paste_            = nullptr;
    QAction* act_find_             = nullptr;
    QAction* act_go_to_line_       = nullptr;
    QAction* act_replace_          = nullptr;
    QAction* act_comment_          = nullptr;
    QAction* act_uncomment_        = nullptr;
    QAction* act_list_members_     = nullptr;
    QAction* act_complete_word_    = nullptr;
    QAction* act_refresh_cache_    = nullptr;

    QAction* act_show_explorer_    = nullptr;
    QAction* act_reg_servers_      = nullptr;
    QAction* act_activity_mon_     = nullptr;
    QAction* act_query_stats_      = nullptr;
    QAction* act_query_history_    = nullptr;
    QAction* act_word_wrap_        = nullptr;
    QAction* act_white_space_      = nullptr;

    QAction* act_conn_connect_     = nullptr;
    QAction* act_conn_disconnect_  = nullptr;
    QAction* act_conn_change_      = nullptr;
    QAction* act_open_in_explorer_ = nullptr;
    QAction* act_execute_          = nullptr;
    QAction* act_cancel_           = nullptr;
    QAction* act_parse_            = nullptr;
    QAction* act_est_plan_         = nullptr;
    QAction* act_intellisense_     = nullptr;
    QAction* act_design_query_     = nullptr;
    QAction* act_actual_plan_      = nullptr;
    QAction* act_client_stats_     = nullptr;
    QAction* act_reset_stats_      = nullptr;
    QActionGroup* act_results_group_ = nullptr;
    QAction* act_results_text_     = nullptr;
    QAction* act_results_grid_     = nullptr;
    QAction* act_results_file_     = nullptr;
    QAction* act_query_options_    = nullptr;
    QAction* act_gen_sql_ai_       = nullptr;

    QAction* act_options_          = nullptr;
    QAction* act_reset_layout_     = nullptr;
    QAction* act_about_            = nullptr;
};

} // namespace capybaradb::ui
