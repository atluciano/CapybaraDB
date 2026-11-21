#include "ui/dialogs/options_dialog.hpp"

#include "core/config/query_history.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace capybaradb::ui {
namespace {

enum OptStackIndex : int {
    kEnvGeneral   = 0,
    kQueryResults = 1,
    kCommands     = 2,
    kHistory      = 3,
    kQueryPages   = 4,
};

QLabel* make_hdr(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setStyleSheet(QStringLiteral("font-weight: 600; color: #1E1E1E; margin-bottom: 4px;"));
    return lbl;
}

QLabel* make_note(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(QStringLiteral("color: #5A5A5A;"));
    return lbl;
}

} // namespace

OptionsDialog::OptionsDialog(std::filesystem::path state_dir, QWidget* parent)
    : QDialog(parent)
    , state_dir_(std::move(state_dir)) {
    setObjectName(QStringLiteral("OptionsDialog"));
    setWindowTitle(QStringLiteral("Options"));
    resize(680, 480);
    setMinimumSize(560, 400);
    setStyleSheet(QStringLiteral("QDialog#OptionsDialog { background-color: #F5F5F5; color: #1E1E1E; }"));

    doc_ = core::load_environment_options(state_dir_ / "environment.json");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setFixedWidth(190);
    tree_->setStyleSheet(QStringLiteral("QTreeWidget { background-color: #FFFFFF; border: 1px solid #C8C8C8; }"));

    auto add_node = [](QTreeWidgetItem* parent_item, const QString& label, int stack_idx, int sub_page = -1) {
        auto* item = new QTreeWidgetItem(parent_item, {label});
        item->setData(0, Qt::UserRole, stack_idx);
        item->setData(0, Qt::UserRole + 1, sub_page);
        return item;
    };

    auto* env_root = new QTreeWidgetItem(tree_, {QStringLiteral("Environment")});
    auto* env_gen  = add_node(env_root, QStringLiteral("General"), kEnvGeneral);
    env_root->setExpanded(true);

    auto* qe_root = new QTreeWidgetItem(tree_, {QStringLiteral("Query Execution")});
    add_node(qe_root, QStringLiteral("General"), kQueryPages, QueryOptionsPagesWidget::ExecutionGeneral);
    add_node(qe_root, QStringLiteral("Advanced"), kQueryPages, QueryOptionsPagesWidget::ExecutionAdvanced);
    add_node(qe_root, QStringLiteral("Query History"), kHistory);
    qe_root->setExpanded(true);

    auto* qr_root = new QTreeWidgetItem(tree_, {QStringLiteral("Query Results")});
    add_node(qr_root, QStringLiteral("General"), kQueryResults);
    add_node(qr_root, QStringLiteral("Grid"), kQueryPages, QueryOptionsPagesWidget::ResultsGrid);
    add_node(qr_root, QStringLiteral("Text"), kQueryPages, QueryOptionsPagesWidget::ResultsText);
    qr_root->setExpanded(true);

    auto* oe_root = new QTreeWidgetItem(tree_, {QStringLiteral("Object Explorer")});
    add_node(oe_root, QStringLiteral("Commands"), kCommands);
    oe_root->setExpanded(true);

    body->addWidget(tree_);

    auto* frame = new QFrame(this);
    frame->setStyleSheet(QStringLiteral("QFrame { background-color: #FFFFFF; border: 1px solid #C8C8C8; }"));
    auto* flay = new QVBoxLayout(frame);
    flay->setContentsMargins(12, 10, 12, 10);

    stack_ = new QStackedWidget(frame);

    // 0: Environment -> General
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(make_hdr(QStringLiteral("Window"), page));
        restore_window_box_ = new QCheckBox(QStringLiteral("Restore window size and position at startup"), page);
        restore_window_box_->setChecked(doc_.restore_window_placement);
        lay->addWidget(restore_window_box_);
        lay->addWidget(make_note(
            QStringLiteral("When on, CapybaraDB opens with the size and position it had when it was closed."), page));
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // 1: Query Results -> General
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(make_hdr(QStringLiteral("General"), page));
        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Default destination for results:"), page), 0, 0);
        default_results_combo_ = new QComboBox(page);
        default_results_combo_->addItems({
            QStringLiteral("Results to grid"),
            QStringLiteral("Results to text"),
            QStringLiteral("Results to file"),
        });
        default_results_combo_->setCurrentIndex(static_cast<int>(doc_.normalized_default_results()));
        default_results_combo_->setFixedWidth(160);
        g->addWidget(default_results_combo_, 0, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);
        lay->addWidget(make_note(
            QStringLiteral("Used by query windows opened from now on. Query ▸ Results To changes only the active tab."), page));
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // 2: Object Explorer -> Commands
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(make_hdr(QStringLiteral("Table and View Options"), page));
        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Value for Select Top <n> Rows command:"), page), 0, 0);
        select_top_edit_ = new QLineEdit(QString::number(doc_.normalized_select_top()), page);
        select_top_edit_->setFixedWidth(96);
        select_top_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(select_top_edit_, 0, 1);

        g->addWidget(new QLabel(QStringLiteral("Value for Edit Top <n> Rows command:"), page), 1, 0);
        edit_top_edit_ = new QLineEdit(QString::number(doc_.normalized_edit_top()), page);
        edit_top_edit_->setFixedWidth(96);
        edit_top_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(edit_top_edit_, 1, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);
        lay->addWidget(make_note(
            QStringLiteral("Number of rows read by Select Top Rows and Edit Top Rows in Object Explorer. Defaults: 1000 and 200."), page));
        commands_error_ = new QLabel(page);
        commands_error_->setStyleSheet(QStringLiteral("color: #B00020;"));
        commands_error_->setVisible(false);
        lay->addWidget(commands_error_);
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // 3: Query Execution -> Query History
    {
        auto* page = new QWidget(stack_);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(make_hdr(QStringLiteral("Query History"), page));
        const core::QueryHistorySettings hs = doc_.normalized_query_history();
        history_enabled_box_ = new QCheckBox(QStringLiteral("Record executed queries in Query History"), page);
        history_enabled_box_->setChecked(hs.enabled);
        lay->addWidget(history_enabled_box_);

        auto* g = new QGridLayout();
        g->addWidget(new QLabel(QStringLiteral("Keep executions for (days):"), page), 0, 0);
        history_days_edit_ = new QLineEdit(QString::number(hs.retention_days), page);
        history_days_edit_->setFixedWidth(96);
        history_days_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(history_days_edit_, 0, 1);

        g->addWidget(new QLabel(QStringLiteral("Maximum executions kept:"), page), 1, 0);
        history_max_edit_ = new QLineEdit(QString::number(hs.max_executions), page);
        history_max_edit_->setFixedWidth(96);
        history_max_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(history_max_edit_, 1, 1);

        g->addWidget(new QLabel(QStringLiteral("Maximum SQL kept per execution (KiB):"), page), 2, 0);
        history_kib_edit_ = new QLineEdit(QString::number(hs.max_sql_kib), page);
        history_kib_edit_->setFixedWidth(96);
        history_kib_edit_->setAlignment(Qt::AlignRight);
        g->addWidget(history_kib_edit_, 2, 1);
        g->setColumnStretch(0, 1);
        lay->addLayout(g);

        lay->addWidget(make_note(
            QStringLiteral("Defaults: 90 days, 10,000 executions, 64 KiB. Stored locally and encrypted per user (DPAPI / libsecret)."), page));

        auto* hbtn_row = new QHBoxLayout();
        auto* clear_btn = new QPushButton(QStringLiteral("Clear History…"), page);
        connect(clear_btn, &QPushButton::clicked, this, &OptionsDialog::on_clear_history);
        history_status_ = new QLabel(page);
        hbtn_row->addWidget(clear_btn);
        hbtn_row->addWidget(history_status_, 1);
        lay->addLayout(hbtn_row);

        history_error_ = new QLabel(page);
        history_error_->setStyleSheet(QStringLiteral("color: #B00020;"));
        history_error_->setVisible(false);
        lay->addWidget(history_error_);
        lay->addStretch(1);
        stack_->addWidget(page);
    }

    // 4: Shared QueryOptionsPagesWidget
    query_pages_ = new QueryOptionsPagesWidget(stack_);
    query_pages_->load_from(doc_.normalized_query_options());
    stack_->addWidget(query_pages_);

    flay->addWidget(stack_);
    body->addWidget(frame, 1);
    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::on_accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (cur == nullptr) return;
        const QVariant idx = cur->data(0, Qt::UserRole);
        if (!idx.isValid()) {
            if (cur->childCount() > 0) tree_->setCurrentItem(cur->child(0));
            return;
        }
        stack_->setCurrentIndex(idx.toInt());
        const int sub = cur->data(0, Qt::UserRole + 1).toInt();
        if (idx.toInt() == kQueryPages && sub >= 0) {
            query_pages_->show_page(sub);
        }
    });
    tree_->setCurrentItem(env_gen);
}

void OptionsDialog::on_clear_history() {
    const auto ans = QMessageBox::question(
        this,
        QStringLiteral("Clear Query History"),
        QStringLiteral("Delete all locally recorded query executions and pins?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (ans != QMessageBox::Yes) return;

    if (core::clear_query_history(state_dir_ / "query-history.dat")) {
        history_status_->setText(QStringLiteral("Query history cleared."));
        emit history_cleared();
    } else {
        history_status_->setText(QStringLiteral("Could not clear query history."));
    }
}

void OptionsDialog::on_accept() {
    commands_error_->setVisible(false);
    history_error_->setVisible(false);

    bool ok = false;
    const int select_top = select_top_edit_->text().trimmed().toInt(&ok);
    if (!ok || select_top < 1 || select_top > 1000000) {
        stack_->setCurrentIndex(kCommands);
        commands_error_->setText(QStringLiteral("Select Top Rows must be between 1 and 1000000."));
        commands_error_->setVisible(true);
        return;
    }
    const int edit_top = edit_top_edit_->text().trimmed().toInt(&ok);
    if (!ok || edit_top < 1 || edit_top > 1000000) {
        stack_->setCurrentIndex(kCommands);
        commands_error_->setText(QStringLiteral("Edit Top Rows must be between 1 and 1000000."));
        commands_error_->setVisible(true);
        return;
    }

    const int h_days = history_days_edit_->text().trimmed().toInt(&ok);
    if (!ok || h_days < core::QueryHistorySettings::kMinRetentionDays ||
        h_days > core::QueryHistorySettings::kMaxRetentionDays) {
        stack_->setCurrentIndex(kHistory);
        history_error_->setText(QStringLiteral("Retention days must be between 1 and 3650."));
        history_error_->setVisible(true);
        return;
    }
    const int h_max = history_max_edit_->text().trimmed().toInt(&ok);
    if (!ok || h_max < core::QueryHistorySettings::kMinExecutions ||
        h_max > core::QueryHistorySettings::kMaxExecutionsLimit) {
        stack_->setCurrentIndex(kHistory);
        history_error_->setText(QStringLiteral("Maximum executions must be between 100 and 1000000."));
        history_error_->setVisible(true);
        return;
    }
    const int h_kib = history_kib_edit_->text().trimmed().toInt(&ok);
    if (!ok || h_kib < core::QueryHistorySettings::kMinSqlKiB ||
        h_kib > core::QueryHistorySettings::kMaxSqlKiBLimit) {
        stack_->setCurrentIndex(kHistory);
        history_error_->setText(QStringLiteral("Maximum SQL kept must be between 1 and 256 KiB."));
        history_error_->setVisible(true);
        return;
    }

    core::QueryOptions qopts;
    if (!query_pages_->try_collect(qopts)) {
        stack_->setCurrentIndex(kQueryPages);
        return;
    }

    const std::filesystem::path env_path = state_dir_ / "environment.json";
    core::EnvironmentDocument d = core::load_environment_options(env_path);
    d.restore_window_placement    = restore_window_box_->isChecked();
    d.default_results_destination = static_cast<core::ResultsDestination>(default_results_combo_->currentIndex());
    d.select_top_rows             = select_top;
    d.edit_top_rows               = edit_top;
    d.default_query_options       = qopts;
    d.show_completion_time        = qopts.show_completion_time;
    d.check_open_transactions     = qopts.check_open_transactions;
    d.query_history               = core::QueryHistorySettings{
        .enabled        = history_enabled_box_->isChecked(),
        .retention_days = h_days,
        .max_executions = h_max,
        .max_sql_kib    = h_kib,
    }.normalized();
    (void)core::save_environment_options(env_path, d);
    doc_ = core::load_environment_options(env_path);
    accept();
}

} // namespace capybaradb::ui
