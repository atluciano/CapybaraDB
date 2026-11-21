#include "ui/dialogs/query_options_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace capybaradb::ui {
namespace {

QLabel* make_section_header(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setStyleSheet(QStringLiteral("font-weight: 600; color: #1E1E1E; margin-bottom: 4px;"));
    return lbl;
}

QLabel* make_hint_label(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(QStringLiteral("color: #5A5A5A;"));
    return lbl;
}

} // namespace

QueryOptionsPagesWidget::QueryOptionsPagesWidget(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    stack_ = new QStackedWidget(this);

    // Page 0: Execution -> General
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(make_section_header(QStringLiteral("Execution ▸ General"), page));

        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Execution time-out (seconds):"), page), 0, 0);
        timeout_edit_ = new QLineEdit(page);
        timeout_edit_->setFixedWidth(96);
        timeout_edit_->setMaxLength(5);
        timeout_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(timeout_edit_, 0, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);
        lay->addWidget(make_hint_label(
            QStringLiteral("0 waits without limit. Same value as Read timeout on the query toolbar."), page));

        check_tx_box_ = new QCheckBox(QStringLiteral("Check for open transactions before closing"), page);
        lay->addWidget(check_tx_box_);
        lay->addWidget(make_hint_label(
            QStringLiteral("Closing a query with a transaction still open asks whether to commit it."), page));
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // Page 1: Execution -> Advanced
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(make_section_header(QStringLiteral("Execution ▸ Advanced"), page));

        nocount_box_    = new QCheckBox(QStringLiteral("Hide row counts (SET NOCOUNT)"), page);
        completion_box_ = new QCheckBox(QStringLiteral("Show completion time"), page);
        lay->addWidget(nocount_box_);
        lay->addWidget(completion_box_);

        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Transaction isolation level:"), page), 0, 0);
        isolation_combo_ = new QComboBox(page);
        isolation_combo_->addItems({
            QStringLiteral("Server default"),
            QStringLiteral("READ COMMITTED"),
            QStringLiteral("REPEATABLE READ"),
            QStringLiteral("SERIALIZABLE"),
        });
        isolation_combo_->setFixedWidth(160);
        g->addWidget(isolation_combo_, 0, 1);

        g->addWidget(new QLabel(QStringLiteral("Lock timeout (milliseconds):"), page), 1, 0);
        lock_timeout_edit_ = new QLineEdit(page);
        lock_timeout_edit_->setFixedWidth(160);
        lock_timeout_edit_->setMaxLength(9);
        lock_timeout_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(lock_timeout_edit_, 1, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);
        lay->addWidget(make_hint_label(
            QStringLiteral("-1 keeps the server's lock timeout; 0 waits without limit."), page));

        lay->addWidget(make_section_header(QStringLiteral("Execution plan: warning for discarded rows"), page));
        auto* pg = new QGridLayout();
        pg->addWidget(new QLabel(QStringLiteral("Discarded rows, in times the rows returned:"), page), 0, 0);
        plan_ratio_edit_ = new QLineEdit(page);
        plan_ratio_edit_->setFixedWidth(120);
        plan_ratio_edit_->setAlignment(Qt::AlignRight);
        pg->addWidget(plan_ratio_edit_, 0, 1);

        pg->addWidget(new QLabel(QStringLiteral("Minimum discarded rows:"), page), 1, 0);
        plan_min_edit_ = new QLineEdit(page);
        plan_min_edit_->setFixedWidth(120);
        plan_min_edit_->setAlignment(Qt::AlignRight);
        pg->addWidget(plan_min_edit_, 1, 1);
        pg->setColumnStretch(0, 1);
        lay->addLayout(pg);
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // Page 2: Results -> Grid
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(make_section_header(QStringLiteral("Results ▸ Grid"), page));

        retain_crlf_box_ = new QCheckBox(QStringLiteral("Retain CR/LF on copy or save"), page);
        lay->addWidget(retain_crlf_box_);
        lay->addWidget(make_hint_label(
            QStringLiteral("When off, each line break in a value becomes a space in the clipboard and in exported files."), page));

        grid_discard_box_ = new QCheckBox(QStringLiteral("Discard results after execution"), page);
        lay->addWidget(grid_discard_box_);

        json_link_box_ = new QCheckBox(QStringLiteral("Display JSON in text columns as Hyperlink"), page);
        lay->addWidget(json_link_box_);
        lay->addWidget(make_hint_label(
            QStringLiteral("Clicking a valid JSON object or array opens it formatted in a new tab."), page));

        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Result arena memory budget (MiB):"), page), 0, 0);
        max_arena_edit_ = new QLineEdit(page);
        max_arena_edit_->setFixedWidth(96);
        max_arena_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(max_arena_edit_, 0, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);
        lay->addWidget(make_hint_label(
            QStringLiteral("Default 1024 MiB (64–16384 MiB). Excess rows are drained without risking OOM."), page));
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // Page 3: Results -> Text
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(make_section_header(QStringLiteral("Results ▸ Text"), page));

        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Output format:"), page), 0, 0);
        text_format_combo_ = new QComboBox(page);
        text_format_combo_->addItems({
            QStringLiteral("Column aligned"),
            QStringLiteral("Comma delimited"),
            QStringLiteral("Tab delimited"),
            QStringLiteral("Space delimited"),
            QStringLiteral("Custom delimiter"),
        });
        text_format_combo_->setFixedWidth(160);
        g->addWidget(text_format_combo_, 0, 1);

        g->addWidget(new QLabel(QStringLiteral("Custom delimiter:"), page), 1, 0);
        custom_delim_edit_ = new QLineEdit(page);
        custom_delim_edit_->setFixedWidth(160);
        custom_delim_edit_->setMaxLength(1);
        g->addWidget(custom_delim_edit_, 1, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);

        text_headers_box_     = new QCheckBox(QStringLiteral("Include column headers in the result set"), page);
        text_query_box_       = new QCheckBox(QStringLiteral("Include the query in the result set"), page);
        text_right_align_box_ = new QCheckBox(QStringLiteral("Right align numeric values"), page);
        text_discard_box_     = new QCheckBox(QStringLiteral("Discard results after execution"), page);
        lay->addWidget(text_headers_box_);
        lay->addWidget(text_query_box_);
        lay->addWidget(text_right_align_box_);
        lay->addWidget(text_discard_box_);

        auto* mg = new QGridLayout();
        mg->addWidget(new QLabel(QStringLiteral("Maximum number of characters displayed in each column:"), page), 0, 0);
        text_max_chars_edit_ = new QLineEdit(page);
        text_max_chars_edit_->setFixedWidth(96);
        text_max_chars_edit_->setMaxLength(4);
        text_max_chars_edit_->setAlignment(Qt::AlignRight);
        mg->addWidget(text_max_chars_edit_, 0, 1);
        mg->setColumnStretch(0, 1);
        lay->addLayout(mg);
        lay->addStretch(1);
        stack_->addWidget(page);

        connect(text_format_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
            update_delimiter_enabled();
        });
    }

    root->addWidget(stack_, 1);

    error_lbl_ = new QLabel(this);
    error_lbl_->setWordWrap(true);
    error_lbl_->setStyleSheet(QStringLiteral("color: #B00020;"));
    error_lbl_->setVisible(false);
    root->addWidget(error_lbl_);

    auto* reset_row = new QHBoxLayout();
    reset_row->addStretch(1);
    auto* reset_btn = new QPushButton(QStringLiteral("Reset to Default"), this);
    connect(reset_btn, &QPushButton::clicked, this, &QueryOptionsPagesWidget::reset_current_page_to_default);
    reset_row->addWidget(reset_btn);
    root->addLayout(reset_row);
}

void QueryOptionsPagesWidget::load_from(const core::QueryOptions& raw) {
    const core::QueryOptions o = raw.normalized();
    timeout_edit_->setText(QString::number(o.execution_timeout_seconds));
    check_tx_box_->setChecked(o.check_open_transactions);

    nocount_box_->setChecked(o.nocount);
    completion_box_->setChecked(o.show_completion_time);
    isolation_combo_->setCurrentIndex(static_cast<int>(o.isolation));
    lock_timeout_edit_->setText(QString::number(o.lock_timeout_ms));
    plan_ratio_edit_->setText(QString::number(o.plan_warning_ratio));
    plan_min_edit_->setText(QString::number(o.plan_warning_min_rows));

    retain_crlf_box_->setChecked(o.retain_line_breaks);
    grid_discard_box_->setChecked(o.grid_discard_results);
    json_link_box_->setChecked(o.display_json_as_link);
    max_arena_edit_->setText(QString::number(o.max_arena_mib));

    text_format_combo_->setCurrentIndex(static_cast<int>(o.text_format));
    custom_delim_edit_->setText(QString(QChar::fromLatin1(o.custom_delimiter)));
    text_headers_box_->setChecked(o.text_include_headers);
    text_query_box_->setChecked(o.text_include_query);
    text_right_align_box_->setChecked(o.text_right_align_numeric);
    text_discard_box_->setChecked(o.text_discard_results);
    text_max_chars_edit_->setText(QString::number(o.text_max_characters));
    error_lbl_->setVisible(false);
    update_delimiter_enabled();
}

void QueryOptionsPagesWidget::show_page(int page_index) {
    if (page_index >= 0 && page_index < stack_->count()) {
        stack_->setCurrentIndex(page_index);
    }
}

void QueryOptionsPagesWidget::reset_current_page_to_default() {
    const core::QueryOptions def{};
    switch (stack_->currentIndex()) {
    case ExecutionGeneral:
        timeout_edit_->setText(QString::number(def.execution_timeout_seconds));
        check_tx_box_->setChecked(def.check_open_transactions);
        break;
    case ExecutionAdvanced:
        nocount_box_->setChecked(def.nocount);
        completion_box_->setChecked(def.show_completion_time);
        isolation_combo_->setCurrentIndex(static_cast<int>(def.isolation));
        lock_timeout_edit_->setText(QString::number(def.lock_timeout_ms));
        plan_ratio_edit_->setText(QString::number(def.plan_warning_ratio));
        plan_min_edit_->setText(QString::number(def.plan_warning_min_rows));
        break;
    case ResultsGrid:
        retain_crlf_box_->setChecked(def.retain_line_breaks);
        grid_discard_box_->setChecked(def.grid_discard_results);
        json_link_box_->setChecked(def.display_json_as_link);
        max_arena_edit_->setText(QString::number(def.max_arena_mib));
        break;
    case ResultsText:
        text_format_combo_->setCurrentIndex(static_cast<int>(def.text_format));
        custom_delim_edit_->setText(QString(QChar::fromLatin1(def.custom_delimiter)));
        text_headers_box_->setChecked(def.text_include_headers);
        text_query_box_->setChecked(def.text_include_query);
        text_right_align_box_->setChecked(def.text_right_align_numeric);
        text_discard_box_->setChecked(def.text_discard_results);
        text_max_chars_edit_->setText(QString::number(def.text_max_characters));
        update_delimiter_enabled();
        break;
    default:
        break;
    }
    error_lbl_->setVisible(false);
}

void QueryOptionsPagesWidget::update_delimiter_enabled() {
    const bool custom = (text_format_combo_->currentIndex() == static_cast<int>(core::TextOutputFormat::CustomDelimited));
    custom_delim_edit_->setEnabled(custom);
    const bool aligned = (text_format_combo_->currentIndex() == static_cast<int>(core::TextOutputFormat::ColumnAligned));
    text_right_align_box_->setEnabled(aligned);
}

bool QueryOptionsPagesWidget::try_collect(core::QueryOptions& out) {
    error_lbl_->setVisible(false);
    auto fail = [&](int page, const QString& msg) -> bool {
        stack_->setCurrentIndex(page);
        error_lbl_->setText(msg);
        error_lbl_->setVisible(true);
        return false;
    };

    bool ok = false;
    const int timeout = timeout_edit_->text().trimmed().toInt(&ok);
    if (!ok || timeout < 0 || timeout > core::QueryOptions::kMaxTimeoutSeconds) {
        return fail(ExecutionGeneral, QStringLiteral("Execution time-out must be between 0 and 86400 seconds."));
    }
    const int lock_ms = lock_timeout_edit_->text().trimmed().toInt(&ok);
    if (!ok || lock_ms < -1 || lock_ms > core::QueryOptions::kMaxLockTimeoutMs) {
        return fail(ExecutionAdvanced, QStringLiteral("Lock timeout must be between -1 and 86400000 milliseconds."));
    }
    const int ratio = plan_ratio_edit_->text().trimmed().toInt(&ok);
    if (!ok || ratio < core::ExecutionPlanWarningOptions::kMinRatio || ratio > core::ExecutionPlanWarningOptions::kMaxRatio) {
        return fail(ExecutionAdvanced, QStringLiteral("Discarded rows ratio must be between 2 and 1000000."));
    }
    const int min_rows = plan_min_edit_->text().trimmed().toInt(&ok);
    if (!ok || min_rows < 0 || min_rows > core::ExecutionPlanWarningOptions::kMaxMinimum) {
        return fail(ExecutionAdvanced, QStringLiteral("Minimum discarded rows must be between 0 and 1000000000."));
    }
    const int arena_mib = max_arena_edit_->text().trimmed().toInt(&ok);
    if (!ok || arena_mib < static_cast<int>(core::QueryOptions::kMinMaxArenaMiB) ||
        arena_mib > static_cast<int>(core::QueryOptions::kMaxMaxArenaMiB)) {
        return fail(ResultsGrid, QStringLiteral("Result arena memory budget must be between 64 and 16384 MiB."));
    }
    const int max_chars = text_max_chars_edit_->text().trimmed().toInt(&ok);
    if (!ok || max_chars < static_cast<int>(core::TextResultsFormat::kMinMaxCharacters) ||
        max_chars > static_cast<int>(core::TextResultsFormat::kMaxMaxCharacters)) {
        return fail(ResultsText, QStringLiteral("Maximum characters displayed per column must be between 1 and 8192."));
    }
    const QString delim = custom_delim_edit_->text();
    if (text_format_combo_->currentIndex() == static_cast<int>(core::TextOutputFormat::CustomDelimited) && delim.size() != 1) {
        return fail(ResultsText, QStringLiteral("Enter a single character for the custom delimiter."));
    }

    out.execution_timeout_seconds = timeout;
    out.check_open_transactions   = check_tx_box_->isChecked();
    out.nocount                   = nocount_box_->isChecked();
    out.show_completion_time      = completion_box_->isChecked();
    out.isolation                 = static_cast<core::QueryIsolationLevel>(isolation_combo_->currentIndex());
    out.lock_timeout_ms           = lock_ms;
    out.plan_warning_ratio        = ratio;
    out.plan_warning_min_rows     = min_rows;
    out.retain_line_breaks        = retain_crlf_box_->isChecked();
    out.grid_discard_results      = grid_discard_box_->isChecked();
    out.display_json_as_link      = json_link_box_->isChecked();
    out.max_arena_mib             = static_cast<uint32_t>(arena_mib);
    out.text_format               = static_cast<core::TextOutputFormat>(text_format_combo_->currentIndex());
    out.custom_delimiter          = (delim.size() == 1) ? delim[0].toLatin1() : ';';
    out.text_include_headers      = text_headers_box_->isChecked();
    out.text_include_query        = text_query_box_->isChecked();
    out.text_right_align_numeric  = text_right_align_box_->isChecked();
    out.text_discard_results      = text_discard_box_->isChecked();
    out.text_max_characters       = static_cast<uint32_t>(max_chars);
    out.normalize_in_place();
    return true;
}

QueryOptionsDialog::QueryOptionsDialog(const core::QueryOptions& initial, QWidget* parent)
    : QDialog(parent)
    , result_(initial.normalized()) {
    setObjectName(QStringLiteral("QueryOptionsDialog"));
    setWindowTitle(QStringLiteral("Query Options"));
    resize(640, 470);
    setMinimumSize(520, 400);
    setStyleSheet(QStringLiteral("QDialog#QueryOptionsDialog { background-color: #F5F5F5; color: #1E1E1E; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setFixedWidth(175);
    tree_->setStyleSheet(QStringLiteral("QTreeWidget { background-color: #FFFFFF; border: 1px solid #C8C8C8; }"));

    auto* exec_root = new QTreeWidgetItem(tree_, {QStringLiteral("Execution")});
    auto* exec_gen  = new QTreeWidgetItem(exec_root, {QStringLiteral("General")});
    exec_gen->setData(0, Qt::UserRole, static_cast<int>(QueryOptionsPagesWidget::ExecutionGeneral));
    auto* exec_adv  = new QTreeWidgetItem(exec_root, {QStringLiteral("Advanced")});
    exec_adv->setData(0, Qt::UserRole, static_cast<int>(QueryOptionsPagesWidget::ExecutionAdvanced));
    exec_root->setExpanded(true);

    auto* res_root = new QTreeWidgetItem(tree_, {QStringLiteral("Results")});
    auto* res_grid = new QTreeWidgetItem(res_root, {QStringLiteral("Grid")});
    res_grid->setData(0, Qt::UserRole, static_cast<int>(QueryOptionsPagesWidget::ResultsGrid));
    auto* res_text = new QTreeWidgetItem(res_root, {QStringLiteral("Text")});
    res_text->setData(0, Qt::UserRole, static_cast<int>(QueryOptionsPagesWidget::ResultsText));
    res_root->setExpanded(true);

    body->addWidget(tree_);

    auto* frame = new QFrame(this);
    frame->setStyleSheet(QStringLiteral("QFrame { background-color: #FFFFFF; border: 1px solid #C8C8C8; }"));
    auto* frame_lay = new QVBoxLayout(frame);
    frame_lay->setContentsMargins(12, 10, 12, 10);

    pages_ = new QueryOptionsPagesWidget(frame);
    pages_->load_from(result_);
    frame_lay->addWidget(pages_);
    body->addWidget(frame, 1);

    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QueryOptionsDialog::on_accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (cur == nullptr) return;
        const QVariant v = cur->data(0, Qt::UserRole);
        if (v.isValid()) {
            pages_->show_page(v.toInt());
        } else if (cur->childCount() > 0) {
            tree_->setCurrentItem(cur->child(0));
        }
    });
    tree_->setCurrentItem(exec_gen);
}

void QueryOptionsDialog::on_accept() {
    if (!pages_->try_collect(result_)) {
        return;
    }
    accept();
}

} // namespace capybaradb::ui
