#pragma once

#include "core/config/query_options.hpp"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTreeWidget;

namespace capybaradb::ui {

/// Reusable 4-page Query Options editor widget shared between QueryOptionsDialog and OptionsDialog.
class QueryOptionsPagesWidget final : public QWidget {
    Q_OBJECT

public:
    enum PageIndex : int {
        ExecutionGeneral  = 0,
        ExecutionAdvanced = 1,
        ResultsGrid       = 2,
        ResultsText       = 3,
    };

    explicit QueryOptionsPagesWidget(QWidget* parent = nullptr);

    void load_from(const core::QueryOptions& opts);
    void show_page(int page_index);
    void reset_current_page_to_default();

    /// Validates all fields and returns normalized QueryOptions, or sets error label and returns false.
    [[nodiscard]] bool try_collect(core::QueryOptions& out);

    [[nodiscard]] QLineEdit* timeout_edit() const noexcept { return timeout_edit_; }
    [[nodiscard]] QCheckBox* check_open_tx_box() const noexcept { return check_tx_box_; }
    [[nodiscard]] QCheckBox* nocount_box() const noexcept { return nocount_box_; }
    [[nodiscard]] QCheckBox* completion_time_box() const noexcept { return completion_box_; }
    [[nodiscard]] QComboBox* isolation_combo() const noexcept { return isolation_combo_; }
    [[nodiscard]] QLineEdit* lock_timeout_edit() const noexcept { return lock_timeout_edit_; }
    [[nodiscard]] QLineEdit* plan_ratio_edit() const noexcept { return plan_ratio_edit_; }
    [[nodiscard]] QLineEdit* plan_min_rows_edit() const noexcept { return plan_min_edit_; }
    [[nodiscard]] QCheckBox* retain_crlf_box() const noexcept { return retain_crlf_box_; }
    [[nodiscard]] QCheckBox* grid_discard_box() const noexcept { return grid_discard_box_; }
    [[nodiscard]] QCheckBox* json_link_box() const noexcept { return json_link_box_; }
    [[nodiscard]] QLineEdit* max_arena_mib_edit() const noexcept { return max_arena_edit_; }
    [[nodiscard]] QComboBox* text_format_combo() const noexcept { return text_format_combo_; }
    [[nodiscard]] QLineEdit* custom_delim_edit() const noexcept { return custom_delim_edit_; }
    [[nodiscard]] QCheckBox* text_headers_box() const noexcept { return text_headers_box_; }
    [[nodiscard]] QCheckBox* text_query_box() const noexcept { return text_query_box_; }
    [[nodiscard]] QCheckBox* text_right_align_box() const noexcept { return text_right_align_box_; }
    [[nodiscard]] QCheckBox* text_discard_box() const noexcept { return text_discard_box_; }
    [[nodiscard]] QLineEdit* text_max_chars_edit() const noexcept { return text_max_chars_edit_; }

private:
    void update_delimiter_enabled();

    QStackedWidget* stack_     = nullptr;
    QLabel*         error_lbl_ = nullptr;

    // Execution -> General
    QLineEdit* timeout_edit_  = nullptr;
    QCheckBox* check_tx_box_  = nullptr;

    // Execution -> Advanced
    QCheckBox* nocount_box_       = nullptr;
    QCheckBox* completion_box_    = nullptr;
    QComboBox* isolation_combo_   = nullptr;
    QLineEdit* lock_timeout_edit_ = nullptr;
    QLineEdit* plan_ratio_edit_   = nullptr;
    QLineEdit* plan_min_edit_     = nullptr;

    // Results -> Grid
    QCheckBox* retain_crlf_box_  = nullptr;
    QCheckBox* grid_discard_box_ = nullptr;
    QCheckBox* json_link_box_    = nullptr;
    QLineEdit* max_arena_edit_   = nullptr;

    // Results -> Text
    QComboBox* text_format_combo_    = nullptr;
    QLineEdit* custom_delim_edit_    = nullptr;
    QCheckBox* text_headers_box_     = nullptr;
    QCheckBox* text_query_box_       = nullptr;
    QCheckBox* text_right_align_box_ = nullptr;
    QCheckBox* text_discard_box_     = nullptr;
    QLineEdit* text_max_chars_edit_  = nullptr;
};

/// Modal Query -> Query Options… dialog (640 x 470, per-tab QueryOptions).
class QueryOptionsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit QueryOptionsDialog(const core::QueryOptions& initial, QWidget* parent = nullptr);

    [[nodiscard]] const core::QueryOptions& options() const noexcept { return result_; }
    [[nodiscard]] QueryOptionsPagesWidget* pages_widget() const noexcept { return pages_; }
    [[nodiscard]] QTreeWidget* tree_widget() const noexcept { return tree_; }

private:
    void on_accept();

    core::QueryOptions       result_;
    QTreeWidget*             tree_  = nullptr;
    QueryOptionsPagesWidget* pages_ = nullptr;
};

} // namespace capybaradb::ui
