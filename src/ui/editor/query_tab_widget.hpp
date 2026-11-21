#pragma once

#include "core/completion/sql_completion.hpp"
#include "core/config/environment_options.hpp"
#include "core/config/query_history.hpp"
#include "core/config/query_options.hpp"
#include "core/plan/execution_plan.hpp"
#include "core/result/result_exporter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/cancel_token.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "core/threading/db_session_worker.hpp"
#include "core/threading/stream_sink.hpp"
#include "drivers/common/catalog_provider.hpp"
#include "drivers/common/db_driver.hpp"
#include "drivers/common/db_types.hpp"
#include "ui/grid/grid_view_state.hpp"

#include <QWidget>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;

namespace capybaradb::ui {

class CompletionPopupWidget;
class ExecutionPlanWidget;
class ExportFlyoutWidget;
class FindReplaceBar;
class GoToLineBar;
class ResultGridViewport;
class SqlCodeEditor;

class QueryTabWidget final : public QWidget {
    Q_OBJECT

public:
    enum class StatusKind { Idle, Running, Success, Error, Canceled };

    explicit QueryTabWidget(
        capybaradb::core::ResultsDestination default_destination = capybaradb::core::ResultsDestination::Grid,
        QWidget* parent = nullptr);
    ~QueryTabWidget() override;

    [[nodiscard]] bool is_connected() const noexcept { return connected_; }
    [[nodiscard]] bool is_busy() const noexcept { return busy_; }
    [[nodiscard]] bool is_canceling() const noexcept { return canceling_; }
    [[nodiscard]] bool is_exporting() const noexcept { return exporting_; }
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }
    void set_dirty(bool dirty);

    [[nodiscard]] const QString& file_path() const noexcept { return file_path_; }
    void set_file_path(const QString& path) { file_path_ = path; }

    [[nodiscard]] const std::string& draft_id() const noexcept { return draft_id_; }
    void set_draft_id(std::string id) { draft_id_ = std::move(id); }

    void set_worker_pool(core::CatalogWorkerPool* pool) noexcept { worker_pool_ = pool; }

    [[nodiscard]] const core::QueryOptions& query_options() const noexcept { return query_options_; }
    void set_query_options(const core::QueryOptions& opts);

    [[nodiscard]] const std::optional<drivers::ConnectionConfig>& connection_config() const noexcept { return connection_config_; }
    [[nodiscard]] const drivers::SessionInfo& session_info() const noexcept { return session_info_; }
    void set_connected_state(const drivers::ConnectionConfig& cfg, const drivers::SessionInfo& info);
    void connect_async(
        const drivers::ConnectionConfig& cfg,
        core::CatalogWorkerPool* worker_pool,
        std::function<void(bool ok, const QString& err)> on_finished = nullptr);
    void disconnect_session();
    void switch_database_async(
        const QString& new_database,
        core::CatalogWorkerPool* worker_pool,
        std::function<bool(const QString& warning)> confirm_loss_risk = nullptr);
    void set_driver_factory(std::function<std::unique_ptr<drivers::DbDriver>(drivers::EngineKind)> factory);
    void set_catalog_provider_factory(std::function<std::unique_ptr<drivers::CatalogProvider>(drivers::EngineKind)> factory);

    // Test hooks (avoid modal native dialogs during headless automated tests)
    void set_export_file_picker(std::function<QString(core::ExportFormat)> picker) {
        export_file_picker_ = std::move(picker);
    }
    void set_results_file_picker(std::function<QString()> picker) {
        results_file_picker_ = std::move(picker);
    }
    void set_analyze_confirm_handler(std::function<bool(const QString&)> handler) {
        analyze_confirm_handler_ = std::move(handler);
    }
    void set_completion_catalog_for_test(core::SqlCompletionCatalog catalog);

    void execute_query();
    void parse_query();
    void cancel_query();
    void run_estimated_plan();
    void run_analyze_plan(bool skip_confirm = false);
    void start_export(core::ExportOptions opts);
    void cancel_export();
    void trigger_intellisense(bool automatic = false);
    void refresh_intellisense_cache();
    void navigate_to_error();
    void comment_selection(bool uncomment);

    /// Checks whether the connected session currently holds an uncommitted/aborted transaction or temp tables.
    std::expected<drivers::SessionLossRisk, drivers::DbError> inspect_loss_risk_sync();
    std::expected<void, drivers::DbError> commit_transaction_sync();

    void show_find_bar();
    void show_replace_bar();
    void show_go_to_line_bar();
    void find_next();
    bool handle_escape_key();

    [[nodiscard]] capybaradb::core::ResultsDestination results_destination() const noexcept { return results_dest_; }
    void set_results_destination(capybaradb::core::ResultsDestination dest);

    [[nodiscard]] bool include_actual_plan() const noexcept { return include_actual_plan_; }
    void set_include_actual_plan(bool enabled);

    [[nodiscard]] bool include_client_statistics() const noexcept { return include_client_stats_; }
    void set_include_client_statistics(bool enabled);

    [[nodiscard]] bool intellisense_enabled() const noexcept { return intellisense_enabled_; }
    void set_intellisense_enabled(bool enabled);

    [[nodiscard]] bool word_wrap() const;
    void set_word_wrap(bool enabled);

    [[nodiscard]] bool show_white_space() const;
    void set_show_white_space(bool enabled);

    [[nodiscard]] bool is_results_pane_visible() const;
    void set_results_pane_visible(bool visible);
    void toggle_results_pane();

    [[nodiscard]] QString editor_text() const;
    void set_editor_text(const QString& text);
    void focus_editor();

    [[nodiscard]] SqlCodeEditor* editor_widget() const noexcept { return editor_; }
    [[nodiscard]] GoToLineBar* go_to_line_bar() const noexcept { return go_to_line_bar_; }
    [[nodiscard]] FindReplaceBar* find_replace_bar() const noexcept { return find_replace_bar_; }
    [[nodiscard]] CompletionPopupWidget* completion_popup() const noexcept { return completion_popup_; }
    [[nodiscard]] ExecutionPlanWidget* execution_plan_widget() const noexcept { return plan_widget_; }
    [[nodiscard]] QWidget* output_panel_widget() const noexcept { return output_panel_; }
    [[nodiscard]] QWidget* output_strip_bar() const noexcept { return output_strip_bar_; }
    [[nodiscard]] QTabBar* output_tab_bar() const noexcept { return output_tab_bar_; }
    [[nodiscard]] ResultGridViewport* result_grid() const noexcept { return result_grid_; }
    [[nodiscard]] QTabBar* result_sets_strip() const noexcept { return result_sets_strip_; }
    [[nodiscard]] const std::shared_ptr<core::QueryExecutionResult>& current_result() const noexcept { return current_result_; }
    [[nodiscard]] QPlainTextEdit* messages_view() const noexcept { return messages_view_; }
    [[nodiscard]] QPlainTextEdit* results_text_view() const noexcept { return results_text_view_; }
    [[nodiscard]] QWidget* query_status_bar() const noexcept { return status_bar_; }
    [[nodiscard]] ExportFlyoutWidget* export_flyout() const noexcept { return export_flyout_; }
    [[nodiscard]] QToolButton* go_to_error_button() const noexcept { return go_to_error_btn_; }
    [[nodiscard]] QToolButton* estimated_plan_button() const noexcept { return est_plan_btn_; }
    [[nodiscard]] QToolButton* analyze_plan_button() const noexcept { return analyze_plan_btn_; }
    [[nodiscard]] QToolButton* cancel_query_button() const noexcept { return cancel_query_btn_; }
    [[nodiscard]] QToolButton* cancel_export_button() const noexcept { return cancel_export_btn_; }
    [[nodiscard]] QToolButton* export_csv_button() const noexcept { return export_csv_btn_; }
    [[nodiscard]] QToolButton* export_flyout_button() const noexcept { return export_flyout_btn_; }
    [[nodiscard]] QLineEdit* timeout_edit() const noexcept { return timeout_edit_; }
    [[nodiscard]] QLabel* status_caret_label() const noexcept { return status_caret_; }
    [[nodiscard]] QLabel* status_message_label() const noexcept { return status_message_; }
    [[nodiscard]] QLabel* status_server_label() const noexcept { return status_server_; }
    [[nodiscard]] QLabel* status_user_label() const noexcept { return status_user_; }
    [[nodiscard]] QLabel* status_database_label() const noexcept { return status_database_; }
    [[nodiscard]] QLabel* status_elapsed_label() const noexcept { return status_elapsed_; }
    [[nodiscard]] QLabel* status_rows_label() const noexcept { return status_rows_; }

    void set_status_state(StatusKind kind, const QString& message, bool error_bg = false);

signals:
    void state_changed();
    void connection_changed(bool connected);
    void execute_requested();
    void cancel_requested();
    void status_hint(const QString& message);
    void open_json_document_requested(const QString& pretty_json);
    void show_plan_in_new_tab_requested(const std::vector<capybaradb::core::ExecutionPlanStatement>& statements);
    void open_script_in_new_query_requested(const QString& sql);
    void query_executed(const capybaradb::core::QueryHistoryCapture& capture);
    void dirty_checkpoint_requested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void build_ui();
    void update_caret_status();
    void update_connection_segments();
    void update_flyout_position();
    void update_destination_ui();
    void update_action_buttons();
    void update_export_availability();
    void toggle_export_flyout(bool open);
    void update_elapsed_clock();
    void update_row_count_segment(uint64_t rows, bool partial);
    void sync_result_sets_ui(const core::QueryExecutionResult& result);
    void switch_active_result_set(int index);
    void on_stream_progress(
        const std::shared_ptr<core::QueryExecutionResult>& result,
        uint64_t gen);
    void set_error_location(int offset, int select_len, uint64_t revision);
    void run_plan_internal(bool analyze);
    void load_completion_catalog_async(bool open_when_ready, bool automatic);
    void show_completion_with_catalog(const core::SqlCompletionCatalog& catalog, bool automatic);
    void update_open_completion_filter();
    void accept_completion_item(const QString& text, int replace_start_char_pos);
    [[nodiscard]] std::string current_completion_cache_key() const;
    [[nodiscard]] core::CatalogWorkerPool& effective_worker_pool();
    [[nodiscard]] QString current_submitted_sql(int& selection_start_out) const;
    [[nodiscard]] bool validate_before_send(
        const QString& sql,
        int selection_start,
        uint64_t revision,
        bool is_parse,
        uint32_t& timeout_out);

    bool connected_            = false;
    bool busy_                 = false;
    bool canceling_            = false;
    bool exporting_            = false;
    bool dirty_                = false;
    bool include_actual_plan_  = false;
    bool include_client_stats_ = false;
    bool intellisense_enabled_ = true;
    capybaradb::core::ResultsDestination results_dest_ = capybaradb::core::ResultsDestination::Grid;
    QString                        file_path_;
    std::string                    draft_id_;
    core::QueryOptions             query_options_;

    std::optional<drivers::ConnectionConfig>           connection_config_;
    drivers::SessionInfo                               session_info_;
    core::DbSessionWorker                              session_worker_;
    core::CatalogWorkerPool*                           worker_pool_ = nullptr;
    std::unique_ptr<core::CatalogWorkerPool>           fallback_pool_;
    std::shared_ptr<core::QueryExecutionResult>        current_result_;
    core::StreamPublishSink                            active_sink_;
    std::vector<std::unique_ptr<grid::GridViewState>>  result_view_states_;
    int                                                active_result_index_ = -1;
    uint64_t                                           op_generation_ = 0;

    std::shared_ptr<core::CancelToken> export_cancel_;
    uint64_t                           export_generation_ = 0;

    core::SqlCompletionCatalog         completion_catalog_;
    bool                               has_completion_catalog_ = false;
    std::string                        completion_cache_key_;
    std::shared_ptr<core::CancelToken> completion_load_cancel_;
    uint64_t                           completion_load_gen_ = 0;
    bool                               completion_loading_ = false;
    bool                               completion_pending_open_ = false;
    bool                               completion_pending_auto_ = false;
    bool                               completion_test_override_ = false;

    std::function<std::unique_ptr<drivers::DbDriver>(drivers::EngineKind)>        driver_factory_;
    std::function<std::unique_ptr<drivers::CatalogProvider>(drivers::EngineKind)> catalog_provider_factory_;
    std::function<QString(core::ExportFormat)>                                     export_file_picker_;
    std::function<QString()>                                                       results_file_picker_;
    std::function<bool(const QString&)>                                            analyze_confirm_handler_;

    std::optional<int> error_offset_;
    int                error_select_len_ = 0;
    uint64_t           error_revision_   = 0;

    QTimer*                               elapsed_timer_          = nullptr;
    QTimer*                               draft_checkpoint_timer_ = nullptr;
    std::chrono::steady_clock::time_point exec_started_;

    // Top tab toolbar
    QLineEdit*   timeout_edit_        = nullptr;
    QToolButton* go_to_error_btn_     = nullptr;
    QToolButton* est_plan_btn_        = nullptr;
    QToolButton* analyze_plan_btn_    = nullptr;
    QToolButton* design_query_btn_    = nullptr;

    // Editor + Inline bars + Completion popup + Splitter + Output
    QSplitter*             splitter_         = nullptr;
    GoToLineBar*           go_to_line_bar_   = nullptr;
    FindReplaceBar*        find_replace_bar_ = nullptr;
    SqlCodeEditor*         editor_           = nullptr;
    CompletionPopupWidget* completion_popup_ = nullptr;
    QWidget*               output_panel_     = nullptr;
    QWidget*               output_strip_bar_ = nullptr;
    QTabBar*               output_tab_bar_   = nullptr;
    QStackedWidget*        output_stack_     = nullptr;

    // Results sub-views & Plan viewer
    QWidget*             results_grid_container_ = nullptr;
    ResultGridViewport*  result_grid_            = nullptr;
    QTabBar*             result_sets_strip_      = nullptr;
    QPlainTextEdit*      results_text_view_      = nullptr;
    QStackedWidget*      results_stack_          = nullptr;
    QPlainTextEdit*      messages_view_          = nullptr;
    ExecutionPlanWidget* plan_widget_            = nullptr;

    // Export quick actions + flyout
    QToolButton*        cancel_query_btn_  = nullptr;
    QToolButton*        cancel_export_btn_ = nullptr;
    QToolButton*        export_csv_btn_    = nullptr;
    QToolButton*        export_flyout_btn_ = nullptr;
    ExportFlyoutWidget* export_flyout_     = nullptr;

    // Query Status Bar (24 px)
    QWidget* status_bar_      = nullptr;
    QLabel*  status_icon_     = nullptr;
    QLabel*  status_message_  = nullptr;
    QLabel*  status_server_   = nullptr;
    QLabel*  status_user_     = nullptr;
    QLabel*  status_database_ = nullptr;
    QLabel*  status_elapsed_  = nullptr;
    QLabel*  status_caret_    = nullptr;
    QLabel*  status_rows_     = nullptr;
};

} // namespace capybaradb::ui
