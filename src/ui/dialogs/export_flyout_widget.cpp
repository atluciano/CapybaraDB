#include "ui/dialogs/export_flyout_widget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace capybaradb::ui {

ExportFlyoutWidget::ExportFlyoutWidget(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("ExportOptionsFlyout"));
    setVisible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(6);
    row1->addWidget(new QLabel(QStringLiteral("Formato:"), this));
    format_combo_ = new QComboBox(this);
    format_combo_->setObjectName(QStringLiteral("ExportFormatCombo"));
    format_combo_->addItems({QStringLiteral("CSV"), QStringLiteral("JSON")});
    row1->addWidget(format_combo_);

    row1->addWidget(new QLabel(QStringLiteral("Escopo:"), this));
    scope_combo_ = new QComboBox(this);
    scope_combo_->setObjectName(QStringLiteral("ExportScopeCombo"));
    scope_combo_->addItems({QStringLiteral("All displayed rows"), QStringLiteral("Selected rows")});
    row1->addWidget(scope_combo_);
    root->addLayout(row1);

    csv_row_ = new QWidget(this);
    auto* row2 = new QHBoxLayout(csv_row_);
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(6);
    row2->addWidget(new QLabel(QStringLiteral("Separator:"), csv_row_));
    delimiter_combo_ = new QComboBox(csv_row_);
    delimiter_combo_->setObjectName(QStringLiteral("ExportDelimiterCombo"));
    delimiter_combo_->addItems({QStringLiteral("Comma"), QStringLiteral("Semicolon"), QStringLiteral("Tab")});
    row2->addWidget(delimiter_combo_);

    headers_box_ = new QCheckBox(QStringLiteral("Headers"), csv_row_);
    headers_box_->setChecked(true);
    row2->addWidget(headers_box_);

    bom_box_ = new QCheckBox(QStringLiteral("UTF-8 BOM"), csv_row_);
    bom_box_->setChecked(false);
    row2->addWidget(bom_box_);
    root->addWidget(csv_row_);

    auto* row3 = new QHBoxLayout();
    row3->setSpacing(8);
    summary_label_ = new QLabel(this);
    summary_label_->setStyleSheet(QStringLiteral("color: #444444; font-size: 11px;"));
    row3->addWidget(summary_label_, 1);

    export_btn_ = new QPushButton(QStringLiteral("Exportar…"), this);
    export_btn_->setObjectName(QStringLiteral("ExportFlyoutActionButton"));
    export_btn_->setEnabled(false);
    row3->addWidget(export_btn_);
    root->addLayout(row3);

    connect(format_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        update_ui_state();
        adjustSize();
        emit size_hint_changed();
    });
    connect(scope_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        update_ui_state();
    });
    connect(delimiter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        update_ui_state();
    });
    connect(headers_box_, &QCheckBox::toggled, this, [this](bool) {
        update_ui_state();
    });
    connect(bom_box_, &QCheckBox::toggled, this, [this](bool) {
        update_ui_state();
    });
    connect(export_btn_, &QPushButton::clicked, this, [this]() {
        emit export_requested(current_options());
    });

    update_ui_state();
}

void ExportFlyoutWidget::set_availability(bool has_result, bool has_selection, bool busy_exporting) {
    has_result_     = has_result;
    has_selection_  = has_selection;
    busy_exporting_ = busy_exporting;
    update_ui_state();
}

core::ExportOptions ExportFlyoutWidget::current_options() const {
    core::ExportOptions opts;
    opts.format = (format_combo_->currentIndex() == 1) ? core::ExportFormat::Json : core::ExportFormat::Csv;
    opts.scope  = (scope_combo_->currentIndex() == 1) ? core::ExportScope::SelectedRows : core::ExportScope::AllDisplayedRows;
    switch (delimiter_combo_->currentIndex()) {
    case 1:  opts.csv_delimiter = ';';  break;
    case 2:  opts.csv_delimiter = '\t'; break;
    default: opts.csv_delimiter = ',';  break;
    }
    opts.include_headers = headers_box_->isChecked();
    opts.utf8_bom        = bom_box_->isChecked();
    return opts;
}

void ExportFlyoutWidget::update_ui_state() {
    const bool is_csv      = (format_combo_->currentIndex() == 0);
    const bool is_selected = (scope_combo_->currentIndex() == 1);
    csv_row_->setVisible(is_csv);

    const QString scope_txt = is_selected ? QStringLiteral("Selected rows") : QStringLiteral("Displayed rows only");
    if (is_csv) {
        const QString bom_txt = bom_box_->isChecked() ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8");
        summary_label_->setText(
            QStringLiteral("%1 · NULL = unquoted \\N · %2").arg(scope_txt, bom_txt));
    } else {
        summary_label_->setText(
            QStringLiteral("%1 · columns + rows arrays · NULL = null · UTF-8").arg(scope_txt));
    }

    const bool can_export = has_result_ && !busy_exporting_ && (!is_selected || has_selection_);
    export_btn_->setEnabled(can_export);
}

} // namespace capybaradb::ui
