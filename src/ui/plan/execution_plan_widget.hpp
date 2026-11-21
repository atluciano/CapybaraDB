#pragma once

#include "core/plan/execution_plan.hpp"

#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QScrollArea;
class QSplitter;
class QTableWidget;
class QToolButton;

namespace capybaradb::ui {

namespace detail {
class PlanDiagramCanvas;
} // namespace detail

/// Graphical CapybaraDB Execution Plan viewer supporting PostgreSQL JSON and SQL Server ShowPlan XML.
class ExecutionPlanWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ExecutionPlanWidget(bool allow_open_in_new_tab = true, QWidget* parent = nullptr);
    ~ExecutionPlanWidget() override = default;

    void set_statements(std::vector<core::ExecutionPlanStatement> statements, const QString& banner_note = {});
    void set_placeholder_message(const QString& message);
    void set_stale(bool stale);
    void clear();

    [[nodiscard]] const std::vector<core::ExecutionPlanStatement>& statements() const noexcept { return statements_; }
    [[nodiscard]] const core::ExecutionPlanOperator* selected_operator() const noexcept { return selected_op_; }
    [[nodiscard]] const core::ExecutionPlanStatement* selected_statement() const noexcept { return selected_stmt_; }
    [[nodiscard]] int operator_count() const;
    [[nodiscard]] double zoom() const noexcept;
    void set_zoom(double factor);
    void zoom_to_fit();

    void open_find_bar();
    void close_find_bar();
    [[nodiscard]] bool is_find_bar_visible() const;
    bool find_next_node(bool forward = true);

    [[nodiscard]] QTableWidget* properties_table() const noexcept { return properties_table_; }
    [[nodiscard]] QComboBox* find_property_combo() const noexcept { return find_property_; }
    [[nodiscard]] QComboBox* find_comparison_combo() const noexcept { return find_comparison_; }
    [[nodiscard]] QLineEdit* find_value_edit() const noexcept { return find_value_; }
    [[nodiscard]] QToolButton* missing_index_button() const noexcept { return missing_idx_btn_; }
    [[nodiscard]] QLabel* stale_label() const noexcept { return stale_label_; }

    /// Test hook to supply save path without native QFileDialog.
    void set_save_file_picker(std::function<QString(const QString& suggested_filter)> picker) {
        save_file_picker_ = std::move(picker);
    }

    void save_selected_plan();
    void request_missing_index_script();

signals:
    void selection_changed();
    void show_in_new_tab_requested(const std::vector<capybaradb::core::ExecutionPlanStatement>& statements);
    void open_script_in_new_query_requested(const QString& sql);
    void status_message(const QString& message);

private:
    void build_ui();
    void on_operator_selected(const core::ExecutionPlanOperator* op, const core::ExecutionPlanStatement* stmt);
    void populate_properties(const core::ExecutionPlanOperator* op);
    void open_custom_zoom_dialog();
    void update_action_states();
    [[nodiscard]] std::vector<core::PlanIndexSuggestion> active_missing_indexes() const;

    bool                                      allow_open_in_new_tab_ = true;
    std::vector<core::ExecutionPlanStatement> statements_;
    const core::ExecutionPlanOperator*        selected_op_   = nullptr;
    const core::ExecutionPlanStatement*       selected_stmt_ = nullptr;
    std::function<QString(const QString&)>    save_file_picker_;

    QLabel*                     placeholder_label_ = nullptr;
    QLabel*                     stale_label_       = nullptr;
    QFrame*                     find_bar_          = nullptr;
    QComboBox*                  find_property_     = nullptr;
    QComboBox*                  find_comparison_   = nullptr;
    QLineEdit*                  find_value_        = nullptr;
    QLabel*                     find_result_       = nullptr;
    QToolButton*                find_btn_          = nullptr;
    QToolButton*                zoom_in_btn_       = nullptr;
    QToolButton*                zoom_out_btn_      = nullptr;
    QToolButton*                zoom_fit_btn_      = nullptr;
    QToolButton*                save_btn_          = nullptr;
    QToolButton*                new_tab_btn_       = nullptr;
    QToolButton*                missing_idx_btn_   = nullptr;
    QSplitter*                  splitter_          = nullptr;
    QScrollArea*                scroll_area_       = nullptr;
    detail::PlanDiagramCanvas*  canvas_            = nullptr;
    QTableWidget*               properties_table_  = nullptr;
};

} // namespace capybaradb::ui
