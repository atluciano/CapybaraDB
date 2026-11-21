#pragma once

#include "core/config/environment_options.hpp"
#include "ui/dialogs/query_options_dialog.hpp"

#include <QDialog>
#include <filesystem>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace capybaradb::ui {

/// Modal Tools -> Options… dialog (680 x 480) editing environment.json defaults & Query History settings.
class OptionsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit OptionsDialog(std::filesystem::path state_dir, QWidget* parent = nullptr);

    [[nodiscard]] const core::EnvironmentDocument& saved_document() const noexcept { return doc_; }
    [[nodiscard]] QCheckBox* restore_window_box() const noexcept { return restore_window_box_; }
    [[nodiscard]] QComboBox* default_results_combo() const noexcept { return default_results_combo_; }
    [[nodiscard]] QLineEdit* select_top_edit() const noexcept { return select_top_edit_; }
    [[nodiscard]] QLineEdit* edit_top_edit() const noexcept { return edit_top_edit_; }
    [[nodiscard]] QCheckBox* history_enabled_box() const noexcept { return history_enabled_box_; }
    [[nodiscard]] QLineEdit* history_days_edit() const noexcept { return history_days_edit_; }
    [[nodiscard]] QLineEdit* history_max_edit() const noexcept { return history_max_edit_; }
    [[nodiscard]] QLineEdit* history_kib_edit() const noexcept { return history_kib_edit_; }
    [[nodiscard]] QueryOptionsPagesWidget* query_pages() const noexcept { return query_pages_; }

signals:
    void history_cleared();

private:
    void on_accept();
    void on_clear_history();

    std::filesystem::path   state_dir_;
    core::EnvironmentDocument doc_;

    QTreeWidget*             tree_                  = nullptr;
    QStackedWidget*          stack_                 = nullptr;
    QueryOptionsPagesWidget* query_pages_           = nullptr;

    QCheckBox*   restore_window_box_    = nullptr;
    QComboBox*   default_results_combo_ = nullptr;
    QLineEdit*   select_top_edit_       = nullptr;
    QLineEdit*   edit_top_edit_         = nullptr;
    QLabel*      commands_error_        = nullptr;

    QCheckBox*   history_enabled_box_   = nullptr;
    QLineEdit*   history_days_edit_     = nullptr;
    QLineEdit*   history_max_edit_      = nullptr;
    QLineEdit*   history_kib_edit_      = nullptr;
    QLabel*      history_status_        = nullptr;
    QLabel*      history_error_         = nullptr;
};

} // namespace capybaradb::ui
