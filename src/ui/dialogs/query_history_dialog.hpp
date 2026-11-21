#pragma once

#include "core/config/query_history.hpp"

#include <QDialog>
#include <filesystem>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace capybaradb::ui {

/// Query History browser dialog (View -> Query History…, Ctrl+Alt+H).
class QueryHistoryDialog final : public QDialog {
    Q_OBJECT

public:
    explicit QueryHistoryDialog(std::filesystem::path state_dir, QWidget* parent = nullptr);

    void reload();

    [[nodiscard]] QTreeWidget* entries_tree() const noexcept { return tree_; }
    [[nodiscard]] QLineEdit* text_filter_edit() const noexcept { return text_filter_; }
    [[nodiscard]] QComboBox* connection_filter_combo() const noexcept { return conn_filter_; }
    [[nodiscard]] QComboBox* period_filter_combo() const noexcept { return period_filter_; }
    [[nodiscard]] QCheckBox* pinned_only_checkbox() const noexcept { return pinned_only_; }
    [[nodiscard]] QPlainTextEdit* sql_preview() const noexcept { return sql_preview_; }
    [[nodiscard]] QPushButton* open_button() const noexcept { return open_btn_; }
    [[nodiscard]] QPushButton* pin_button() const noexcept { return pin_btn_; }
    [[nodiscard]] QPushButton* delete_button() const noexcept { return delete_btn_; }

    void open_selected();
    void toggle_pin_selected();

signals:
    void open_in_new_query_requested(
        const QString& sql,
        const QString& engine,
        const QString& server,
        int port,
        const QString& database,
        const QString& user);

private:
    void build_ui();
    void apply_filters();
    void update_selection_state();
    void copy_selected_sql();
    void delete_selected();
    void clear_all_history();

    struct SelectionInfo {
        bool                     is_group = true;
        core::QueryHistoryKey    key;
        core::QueryHistoryEntry  entry;
        bool                     pinned   = false;
    };

    [[nodiscard]] std::optional<SelectionInfo> current_selection() const;

    std::filesystem::path             state_dir_;
    std::vector<core::QueryHistoryGroup> all_groups_;

    QLabel*         banner_lbl_     = nullptr;
    QFrame*         disabled_frame_ = nullptr;
    QPushButton*    open_btn_       = nullptr;
    QPushButton*    copy_btn_       = nullptr;
    QPushButton*    pin_btn_        = nullptr;
    QPushButton*    delete_btn_     = nullptr;
    QLineEdit*      text_filter_    = nullptr;
    QComboBox*      conn_filter_    = nullptr;
    QComboBox*      period_filter_  = nullptr;
    QCheckBox*      pinned_only_    = nullptr;
    QTreeWidget*    tree_           = nullptr;
    QPlainTextEdit* sql_preview_    = nullptr;
    QLabel*         details_lbl_    = nullptr;
    QLabel*         status_lbl_     = nullptr;
};

} // namespace capybaradb::ui
