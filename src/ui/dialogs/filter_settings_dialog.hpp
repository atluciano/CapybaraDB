#pragma once

#include "drivers/common/catalog_provider.hpp"

#include <QDialog>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace capybaradb::ui {

class FilterSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    FilterSettingsDialog(
        const QString& server,
        const QString& database,
        const drivers::CatalogFilter& current,
        bool can_filter_by_owner,
        QWidget* parent = nullptr);

    [[nodiscard]] drivers::CatalogFilter filter_result() const { return result_; }

    [[nodiscard]] QTableWidget* criteria_table() const noexcept { return table_; }
    [[nodiscard]] QLineEdit* name_value_edit() const noexcept { return name_edit_; }
    [[nodiscard]] QComboBox* name_operator_combo() const noexcept { return name_op_; }
    [[nodiscard]] QLineEdit* owner_value_edit() const noexcept { return owner_edit_; }
    [[nodiscard]] QComboBox* owner_operator_combo() const noexcept { return owner_op_; }
    [[nodiscard]] QLabel* description_label() const noexcept { return desc_label_; }
    [[nodiscard]] QPushButton* clear_button() const noexcept { return clear_btn_; }
    [[nodiscard]] QPushButton* ok_button() const noexcept { return ok_btn_; }

private:
    void build_ui(
        const QString& server,
        const QString& database,
        const drivers::CatalogFilter& current,
        bool can_filter_by_owner);
    void on_clear_filter();
    void on_accept();

    drivers::CatalogFilter result_;
    QTableWidget*          table_       = nullptr;
    QComboBox*             name_op_     = nullptr;
    QLineEdit*             name_edit_   = nullptr;
    QComboBox*             owner_op_    = nullptr;
    QLineEdit*             owner_edit_  = nullptr;
    QLabel*                desc_label_  = nullptr;
    QPushButton*           clear_btn_   = nullptr;
    QPushButton*           ok_btn_      = nullptr;
    std::vector<QString>   descriptions_;
};

} // namespace capybaradb::ui
