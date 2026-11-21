#include "ui/dialogs/query_history_dialog.hpp"

#include "core/config/environment_options.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimeZone>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <set>

namespace capybaradb::ui {
namespace {

constexpr int kRoleIsGroup  = Qt::UserRole;
constexpr int kRoleGroupIdx = Qt::UserRole + 1;
constexpr int kRoleExecIdx  = Qt::UserRole + 2;

QString format_utc_ms_local(int64_t ms) {
    if (ms <= 0) return QStringLiteral("-");
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::LocalTime).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString format_outcome(core::QueryHistoryOutcome o) {
    switch (o) {
    case core::QueryHistoryOutcome::Succeeded: return QStringLiteral("Succeeded");
    case core::QueryHistoryOutcome::Failed:    return QStringLiteral("Failed");
    case core::QueryHistoryOutcome::Canceled:  return QStringLiteral("Canceled");
    }
    return QStringLiteral("Succeeded");
}

QString format_origin(core::QueryHistoryOrigin o) {
    switch (o) {
    case core::QueryHistoryOrigin::Typed:         return QStringLiteral("Typed");
    case core::QueryHistoryOrigin::QueryDesigner: return QStringLiteral("Query Designer");
    case core::QueryHistoryOrigin::Ai:            return QStringLiteral("AI");
    case core::QueryHistoryOrigin::Other:         return QStringLiteral("Other");
    }
    return QStringLiteral("Typed");
}

QString format_duration_ms(int64_t ms) {
    if (ms < 1000) return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(QString::number(static_cast<double>(ms) / 1000.0, 'f', 2));
}

QString format_rows(const core::QueryHistoryEntry& e) {
    if (e.rows_returned.has_value()) return QString::number(*e.rows_returned);
    if (e.rows_affected.has_value()) return QStringLiteral("%1 aff.").arg(*e.rows_affected);
    return QStringLiteral("-");
}

QString conn_label(const core::QueryHistoryEntry& e) {
    return QStringLiteral("%1:%2 / %3").arg(QString::fromStdString(e.server)).arg(e.port).arg(QString::fromStdString(e.database));
}

} // namespace

QueryHistoryDialog::QueryHistoryDialog(std::filesystem::path state_dir, QWidget* parent)
    : QDialog(parent)
    , state_dir_(std::move(state_dir)) {
    setObjectName(QStringLiteral("QueryHistoryDialog"));
    setWindowTitle(QStringLiteral("Query History"));
    resize(1100, 680);
    setMinimumSize(760, 480);
    setStyleSheet(QStringLiteral("QDialog#QueryHistoryDialog { background-color: #F5F5F5; color: #1E1E1E; }"));
    build_ui();
    reload();
}

void QueryHistoryDialog::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* banner = new QFrame(this);
    banner->setStyleSheet(QStringLiteral("QFrame { background-color: #E4EFF6; border: 1px solid #B5C5D2; }"));
    auto* blay = new QVBoxLayout(banner);
    blay->setContentsMargins(10, 8, 10, 8);
    banner_lbl_ = new QLabel(
        QStringLiteral("Local Query History — encrypted per user on this machine (DPAPI / libsecret). Never written to any server."),
        banner);
    banner_lbl_->setWordWrap(true);
    blay->addWidget(banner_lbl_);
    root->addWidget(banner);

    disabled_frame_ = new QFrame(this);
    disabled_frame_->setStyleSheet(QStringLiteral("QFrame { background-color: #FFF4CE; border: 1px solid #E0C068; }"));
    disabled_frame_->setVisible(false);
    auto* dlay = new QVBoxLayout(disabled_frame_);
    dlay->setContentsMargins(10, 6, 10, 6);
    auto* dlbl = new QLabel(
        QStringLiteral("Recording is turned off in Tools ▸ Options ▸ Query Execution ▸ Query History. Previously recorded executions remain below."),
        disabled_frame_);
    dlbl->setWordWrap(true);
    dlay->addWidget(dlbl);
    root->addWidget(disabled_frame_);

    // Action buttons
    auto* actions = new QHBoxLayout();
    actions->setSpacing(6);
    auto* refresh_btn = new QPushButton(QStringLiteral("Refresh"), this);
    open_btn_         = new QPushButton(QStringLiteral("Open in New Query"), this);
    copy_btn_         = new QPushButton(QStringLiteral("Copy SQL"), this);
    pin_btn_          = new QPushButton(QStringLiteral("Pin"), this);
    delete_btn_       = new QPushButton(QStringLiteral("Delete…"), this);
    auto* clear_btn   = new QPushButton(QStringLiteral("Clear History…"), this);

    open_btn_->setEnabled(false);
    copy_btn_->setEnabled(false);
    pin_btn_->setEnabled(false);
    delete_btn_->setEnabled(false);

    actions->addWidget(refresh_btn);
    actions->addWidget(open_btn_);
    actions->addWidget(copy_btn_);
    actions->addWidget(pin_btn_);
    actions->addWidget(delete_btn_);
    actions->addWidget(clear_btn);
    actions->addStretch(1);
    root->addLayout(actions);

    connect(refresh_btn, &QPushButton::clicked, this, &QueryHistoryDialog::reload);
    connect(open_btn_, &QPushButton::clicked, this, &QueryHistoryDialog::open_selected);
    connect(copy_btn_, &QPushButton::clicked, this, &QueryHistoryDialog::copy_selected_sql);
    connect(pin_btn_, &QPushButton::clicked, this, &QueryHistoryDialog::toggle_pin_selected);
    connect(delete_btn_, &QPushButton::clicked, this, &QueryHistoryDialog::delete_selected);
    connect(clear_btn, &QPushButton::clicked, this, &QueryHistoryDialog::clear_all_history);

    // Filter row
    auto* filters = new QHBoxLayout();
    filters->setSpacing(8);
    filters->addWidget(new QLabel(QStringLiteral("Text contains:"), this));
    text_filter_ = new QLineEdit(this);
    text_filter_->setPlaceholderText(QStringLiteral("Filter SQL, database, or user…"));
    filters->addWidget(text_filter_, 2);

    filters->addWidget(new QLabel(QStringLiteral("Connection:"), this));
    conn_filter_ = new QComboBox(this);
    conn_filter_->setMinimumWidth(180);
    filters->addWidget(conn_filter_, 1);

    filters->addWidget(new QLabel(QStringLiteral("Period:"), this));
    period_filter_ = new QComboBox(this);
    period_filter_->addItem(QStringLiteral("All time"), 0);
    period_filter_->addItem(QStringLiteral("Today"), 1);
    period_filter_->addItem(QStringLiteral("Last 7 days"), 7);
    period_filter_->addItem(QStringLiteral("Last 30 days"), 30);
    filters->addWidget(period_filter_);

    pinned_only_ = new QCheckBox(QStringLiteral("Pinned only"), this);
    filters->addWidget(pinned_only_);
    root->addLayout(filters);

    connect(text_filter_, &QLineEdit::textChanged, this, &QueryHistoryDialog::apply_filters);
    connect(conn_filter_, &QComboBox::currentIndexChanged, this, &QueryHistoryDialog::apply_filters);
    connect(period_filter_, &QComboBox::currentIndexChanged, this, &QueryHistoryDialog::apply_filters);
    connect(pinned_only_, &QCheckBox::toggled, this, &QueryHistoryDialog::apply_filters);

    // Grouped entries tree
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("QueryHistoryTree"));
    tree_->setColumnCount(11);
    tree_->setHeaderLabels({
        QStringLiteral("★"),
        QStringLiteral("Last run"),
        QStringLiteral("Server"),
        QStringLiteral("Database"),
        QStringLiteral("User"),
        QStringLiteral("Runs"),
        QStringLiteral("Duration"),
        QStringLiteral("Rows"),
        QStringLiteral("Result"),
        QStringLiteral("Origin"),
        QStringLiteral("SQL"),
    });
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->resizeSection(0, 34);
    tree_->header()->resizeSection(1, 140);
    tree_->header()->resizeSection(2, 130);
    tree_->header()->resizeSection(3, 100);
    tree_->header()->resizeSection(4, 90);
    tree_->header()->resizeSection(5, 48);
    tree_->header()->resizeSection(6, 75);
    tree_->header()->resizeSection(7, 80);
    tree_->header()->resizeSection(8, 82);
    tree_->header()->resizeSection(9, 95);
    tree_->header()->setStretchLastSection(true);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        update_selection_state();
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) {
        open_selected();
    });
    root->addWidget(tree_, 1);

    sql_preview_ = new QPlainTextEdit(this);
    sql_preview_->setReadOnly(true);
    sql_preview_->setFont(theme::resolve_mono_font(13));
    sql_preview_->setFixedHeight(130);
    root->addWidget(sql_preview_);

    details_lbl_ = new QLabel(this);
    details_lbl_->setWordWrap(true);
    root->addWidget(details_lbl_);

    status_lbl_ = new QLabel(this);
    status_lbl_->setStyleSheet(QStringLiteral("color: #4B5964;"));
    root->addWidget(status_lbl_);
}

void QueryHistoryDialog::reload() {
    const core::EnvironmentDocument env_doc =
        core::load_environment_options(state_dir_ / "environment.json");
    disabled_frame_->setVisible(!env_doc.normalized_query_history().enabled);

    all_groups_ = core::load_query_history_groups(state_dir_ / "query-history.dat");

    const QString prev_conn = conn_filter_->currentText();
    conn_filter_->blockSignals(true);
    conn_filter_->clear();
    conn_filter_->addItem(QStringLiteral("All connections"));
    std::set<QString> conns;
    for (const auto& g : all_groups_) {
        conns.insert(conn_label(g.latest()));
    }
    for (const QString& c : conns) {
        conn_filter_->addItem(c);
    }
    const int prev_idx = conn_filter_->findText(prev_conn);
    conn_filter_->setCurrentIndex(prev_idx >= 0 ? prev_idx : 0);
    conn_filter_->blockSignals(false);

    apply_filters();
}

void QueryHistoryDialog::apply_filters() {
    tree_->clear();
    const std::string needle_norm = core::normalize_query_history_sql(text_filter_->text().toStdString());
    const QString needle_q = QString::fromStdString(needle_norm);
    const QString conn_sel = conn_filter_->currentIndex() > 0 ? conn_filter_->currentText() : QString{};
    const int period_days  = period_filter_->currentData().toInt();
    const bool only_pinned = pinned_only_->isChecked();
    const int64_t now_ms   = core::current_utc_ms();
    const int64_t cutoff   = (period_days > 0) ? (now_ms - static_cast<int64_t>(period_days) * 86400LL * 1000LL) : 0;

    int shown_groups = 0;
    int shown_execs  = 0;

    for (size_t gi = 0; gi < all_groups_.size(); ++gi) {
        const auto& g = all_groups_[gi];
        const auto& latest = g.latest();
        if (only_pinned && !g.pinned) continue;
        if (cutoff > 0 && latest.started_utc_ms < cutoff) continue;
        if (!conn_sel.isEmpty() && conn_label(latest) != conn_sel) continue;
        if (!needle_q.isEmpty()) {
            const QString hay = QString::fromStdString(core::normalize_query_history_sql(latest.sql)) +
                                QLatin1Char(' ') + QString::fromStdString(latest.database) +
                                QLatin1Char(' ') + QString::fromStdString(latest.user);
            if (!hay.contains(needle_q, Qt::CaseInsensitive)) continue;
        }

        auto* top = new QTreeWidgetItem(tree_, {
            g.pinned ? QStringLiteral("★") : QString{},
            format_utc_ms_local(latest.started_utc_ms),
            QStringLiteral("%1:%2").arg(QString::fromStdString(latest.server)).arg(latest.port),
            QString::fromStdString(latest.database),
            QString::fromStdString(latest.user),
            QString::number(g.executions.size()),
            format_duration_ms(latest.duration_ms),
            format_rows(latest),
            format_outcome(latest.outcome),
            format_origin(latest.origin),
            QString::fromStdString(core::preview_query_history_sql(latest.sql)),
        });
        top->setData(0, kRoleIsGroup, true);
        top->setData(0, kRoleGroupIdx, static_cast<int>(gi));
        top->setData(0, kRoleExecIdx, 0);

        if (g.executions.size() > 1) {
            for (size_t ei = 0; ei < g.executions.size(); ++ei) {
                const auto& ex = g.executions[ei];
                auto* ch = new QTreeWidgetItem(top, {
                    QString{},
                    format_utc_ms_local(ex.started_utc_ms),
                    QStringLiteral("%1:%2").arg(QString::fromStdString(ex.server)).arg(ex.port),
                    QString::fromStdString(ex.database),
                    QString::fromStdString(ex.user),
                    QStringLiteral("#%1").arg(ei + 1),
                    format_duration_ms(ex.duration_ms),
                    format_rows(ex),
                    format_outcome(ex.outcome),
                    format_origin(ex.origin),
                    ex.error.has_value() ? QString::fromStdString(*ex.error) : QString{},
                });
                ch->setData(0, kRoleIsGroup, false);
                ch->setData(0, kRoleGroupIdx, static_cast<int>(gi));
                ch->setData(0, kRoleExecIdx, static_cast<int>(ei));
            }
        }

        ++shown_groups;
        shown_execs += static_cast<int>(g.executions.size());
    }

    status_lbl_->setText(QStringLiteral("%1 distinct quer%2 (%3 execution(s)).")
        .arg(shown_groups)
        .arg(shown_groups == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
        .arg(shown_execs));
    if (tree_->topLevelItemCount() > 0) {
        tree_->setCurrentItem(tree_->topLevelItem(0));
    }
    update_selection_state();
}

std::optional<QueryHistoryDialog::SelectionInfo> QueryHistoryDialog::current_selection() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return std::nullopt;
    const int gi = item->data(0, kRoleGroupIdx).toInt();
    const int ei = item->data(0, kRoleExecIdx).toInt();
    if (gi < 0 || static_cast<size_t>(gi) >= all_groups_.size()) return std::nullopt;
    const auto& grp = all_groups_[static_cast<size_t>(gi)];
    if (ei < 0 || static_cast<size_t>(ei) >= grp.executions.size()) return std::nullopt;
    return SelectionInfo{
        .is_group = item->data(0, kRoleIsGroup).toBool(),
        .key      = grp.key,
        .entry    = grp.executions[static_cast<size_t>(ei)],
        .pinned   = grp.pinned,
    };
}

void QueryHistoryDialog::update_selection_state() {
    const auto sel = current_selection();
    const bool has = sel.has_value();
    open_btn_->setEnabled(has);
    copy_btn_->setEnabled(has);
    pin_btn_->setEnabled(has);
    delete_btn_->setEnabled(has);
    if (!has) {
        sql_preview_->clear();
        details_lbl_->clear();
        pin_btn_->setText(QStringLiteral("Pin"));
        return;
    }
    pin_btn_->setText(sel->pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"));
    sql_preview_->setPlainText(QString::fromStdString(sel->entry.reopen_text()));

    QString det = QStringLiteral("%1 · %2:%3 / %4 · %5 · %6")
        .arg(QString::fromStdString(sel->entry.engine),
             QString::fromStdString(sel->entry.server))
        .arg(sel->entry.port)
        .arg(QString::fromStdString(sel->entry.database),
             format_outcome(sel->entry.outcome),
             format_duration_ms(sel->entry.duration_ms));
    if (sel->entry.error.has_value() && !sel->entry.error->empty()) {
        det += QStringLiteral(" · Error: %1").arg(QString::fromStdString(*sel->entry.error));
    }
    details_lbl_->setText(det);
}

void QueryHistoryDialog::open_selected() {
    const auto sel = current_selection();
    if (!sel.has_value()) return;
    emit open_in_new_query_requested(
        QString::fromStdString(sel->entry.reopen_text()),
        QString::fromStdString(sel->entry.engine),
        QString::fromStdString(sel->entry.server),
        sel->entry.port,
        QString::fromStdString(sel->entry.database),
        QString::fromStdString(sel->entry.user));
    accept();
}

void QueryHistoryDialog::copy_selected_sql() {
    const auto sel = current_selection();
    if (!sel.has_value()) return;
    if (QClipboard* cb = QApplication::clipboard()) {
        cb->setText(QString::fromStdString(sel->entry.reopen_text()));
        status_lbl_->setText(QStringLiteral("SQL copied to clipboard."));
    }
}

void QueryHistoryDialog::toggle_pin_selected() {
    const auto sel = current_selection();
    if (!sel.has_value()) return;
    if (core::toggle_query_history_pin(state_dir_ / "query-history.dat", sel->key)) {
        reload();
    }
}

void QueryHistoryDialog::delete_selected() {
    const auto sel = current_selection();
    if (!sel.has_value()) return;
    const QString msg = sel->is_group
        ? QStringLiteral("Delete all executions of this query from Query History?")
        : QStringLiteral("Delete only this execution from Query History?");
    if (QMessageBox::question(this, QStringLiteral("Delete Query History"), msg, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const std::filesystem::path hpath = state_dir_ / "query-history.dat";
    if (sel->is_group) {
        (void)core::delete_query_history_group(hpath, sel->key);
    } else {
        (void)core::delete_query_history_entry(hpath, sel->entry.id);
    }
    reload();
}

void QueryHistoryDialog::clear_all_history() {
    if (QMessageBox::question(
            this,
            QStringLiteral("Clear Query History"),
            QStringLiteral("Delete all locally stored query executions and pins?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    (void)core::clear_query_history(state_dir_ / "query-history.dat");
    reload();
}

} // namespace capybaradb::ui
