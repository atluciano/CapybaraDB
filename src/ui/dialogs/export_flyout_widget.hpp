#pragma once

#include "core/result/result_exporter.hpp"

#include <QFrame>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QWidget;

namespace capybaradb::ui {

/// Compact CapybaraDB export configuration flyout anchored below the OutputTabs strip.
class ExportFlyoutWidget final : public QFrame {
    Q_OBJECT

public:
    explicit ExportFlyoutWidget(QWidget* parent = nullptr);

    void set_availability(bool has_result, bool has_selection, bool busy_exporting = false);

    [[nodiscard]] core::ExportOptions current_options() const;

    [[nodiscard]] QComboBox* format_combo() const noexcept { return format_combo_; }
    [[nodiscard]] QComboBox* scope_combo() const noexcept { return scope_combo_; }
    [[nodiscard]] QComboBox* delimiter_combo() const noexcept { return delimiter_combo_; }
    [[nodiscard]] QCheckBox* headers_checkbox() const noexcept { return headers_box_; }
    [[nodiscard]] QCheckBox* bom_checkbox() const noexcept { return bom_box_; }
    [[nodiscard]] QPushButton* export_button() const noexcept { return export_btn_; }
    [[nodiscard]] QLabel* summary_label() const noexcept { return summary_label_; }

signals:
    void export_requested(const capybaradb::core::ExportOptions& options);
    void size_hint_changed();

private:
    void update_ui_state();

    bool has_result_     = false;
    bool has_selection_  = false;
    bool busy_exporting_ = false;

    QComboBox*   format_combo_    = nullptr;
    QComboBox*   scope_combo_     = nullptr;
    QWidget*     csv_row_         = nullptr;
    QComboBox*   delimiter_combo_ = nullptr;
    QCheckBox*   headers_box_     = nullptr;
    QCheckBox*   bom_box_         = nullptr;
    QLabel*      summary_label_   = nullptr;
    QPushButton* export_btn_      = nullptr;
};

} // namespace capybaradb::ui
