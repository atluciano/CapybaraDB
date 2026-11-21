#include "ui/editor/query_tab_widget.hpp"

#include "core/result/result_formatter.hpp"
#include "dialects/sql_placeholder_scanner.hpp"
#include "drivers/common/driver_registry.hpp"
#include "ui/dialogs/export_flyout_widget.hpp"
#include "ui/editor/completion_popup.hpp"
#include "ui/editor/editor_search_bars.hpp"
#include "ui/editor/sql_code_editor.hpp"
#include "ui/grid/result_grid_viewport.hpp"
#include "ui/plan/execution_plan_widget.hpp"
#include "ui/shell/icon_catalog.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdio>

namespace capybaradb::ui {
namespace {

QString format_hhmmss(int64_t total_seconds) {
    if (total_seconds < 0) {
        total_seconds = 0;
    }
    const int64_t hours = total_seconds / 3600;
    const int64_t mins  = (total_seconds % 3600) / 60;
    const int64_t secs  = total_seconds % 60;
    char buf[32]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%02lld:%02lld:%02lld",
        static_cast<long long>(hours),
        static_cast<long long>(mins),
        static_cast<long long>(secs));
    return QString::fromLatin1(buf);
}

QString format_messages_and_completion(
    const core::QueryExecutionResult& result,
    bool show_completion_time) {
    QString out;
    const auto msgs = result.messages_snapshot();
    for (const core::MessageEntry& m : msgs) {
        out += QLatin1Char('\n');
        if (!m.sql_state.empty()) {
            const char* sev = (m.severity == core::MessageSeverity::Error) ? "ERROR"
                : (m.severity == core::MessageSeverity::Warning) ? "WARNING"
                : (m.severity == core::MessageSeverity::Notice) ? "NOTICE"
                : "INFO";
            out += QString::fromLatin1(sev) + QLatin1Char(' ') +
                   QString::fromUtf8(m.sql_state.data(), static_cast<qsizetype>(m.sql_state.size())) +
                   QStringLiteral(": ") +
                   QString::fromUtf8(m.text.data(), static_cast<qsizetype>(m.text.size()));
        } else {
            out += QString::fromUtf8(m.text.data(), static_cast<qsizetype>(m.text.size()));
        }
    }
    const uint64_t omitted = result.omitted_messages.load(std::memory_order_acquire);
    if (omitted > 0) {
        out += QStringLiteral("\n%1 server notice(s) omitted: limit 100 notices, 4096 characters each, 65536 characters total.")
                   .arg(omitted);
    }
    if (show_completion_time) {
        out += QStringLiteral("\n\n") + QString::fromStdString(core::format_completion_time_line());
    }
    return out;
}

QString format_limit_warnings(uint8_t reasons) {
    QString out;
    if ((reasons & core::kLimitArenaBytes) != 0) {
        out += QStringLiteral("\nDisplay memory budget reached; additional rows were drained without storing.");
    }
    if ((reasons & core::kLimitResultSets) != 0) {
        out += QStringLiteral("\nResult set display limit (100) reached.");
    }
    if ((reasons & core::kLimitColumns) != 0) {
        out += QStringLiteral("\nColumn display limit reached; extra columns were omitted.");
    }
    if ((reasons & core::kLimitCellBytes) != 0) {
        out += QStringLiteral("\nOne or more cell values exceeded 64 KiB and were truncated for display.");
    }
    return out;
}

QString format_byte_size(uint64_t bytes) {
    if (bytes < 1024u) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024u * 1024u) {
        return QStringLiteral("%1 KiB").arg(QString::number(static_cast<double>(bytes) / 1024.0, 'f', 1));
    }
    return QStringLiteral("%1 MiB").arg(QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 2));
}

bool is_completion_filter_char(QChar ch) {
    return ch.isLetterOrNumber() || ch == u'_' || ch == u'$' || ch == u'"' || ch == u'[' || ch == u']';
}

std::string engine_to_history_name(drivers::EngineKind engine) {
    return std::string(drivers::engine_kind_to_string(engine));
}

} // namespace

QueryTabWidget::QueryTabWidget(
    capybaradb::core::ResultsDestination default_destination,
    QWidget* parent)
    : QWidget(parent)
    , results_dest_(default_destination)
    , draft_id_(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()) {
    driver_factory_ = [](drivers::EngineKind engine) {
        return drivers::create_db_driver(engine);
    };
    catalog_provider_factory_ = [](drivers::EngineKind engine) {
        return drivers::create_catalog_provider(engine);
    };
    setObjectName(QStringLiteral("QueryTabWidget"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    build_ui();
    set_query_options(query_options_);
    update_destination_ui();
    update_action_buttons();
    update_export_availability();
    update_caret_status();
    update_connection_segments();
    set_status_state(StatusKind::Idle, QStringLiteral("Não conectado"), false);
}

QueryTabWidget::~QueryTabWidget() {
    if (elapsed_timer_ != nullptr) {
        elapsed_timer_->stop();
    }
    if (draft_checkpoint_timer_ != nullptr) {
        draft_checkpoint_timer_->stop();
    }
    if (export_cancel_) {
        export_cancel_->cancel();
    }
    if (completion_load_cancel_) {
        completion_load_cancel_->cancel();
    }
    active_sink_.clear_callback();
    active_sink_.clear_pending();
    session_worker_.stop();
    if (fallback_pool_) {
        fallback_pool_->stop();
        fallback_pool_.reset();
    }
    if (result_grid_ != nullptr) {
        result_grid_->clear();
    }
    result_view_states_.clear();
    current_result_.reset();
}

void QueryTabWidget::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Top Toolbar (#D5E5EF)
    auto* top_bar = new QWidget(this);
    top_bar->setObjectName(QStringLiteral("QueryTabToolbar"));
    top_bar->setAttribute(Qt::WA_StyledBackground, true);
    top_bar->setMinimumWidth(0);
    top_bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* tb = new QHBoxLayout(top_bar);
    tb->setContentsMargins(6, 3, 6, 3);
    tb->setSpacing(6);

    tb->addWidget(new QLabel(QStringLiteral("Read timeout (s):"), top_bar));
    timeout_edit_ = new QLineEdit(QStringLiteral("0"), top_bar);
    timeout_edit_->setFixedWidth(65);
    timeout_edit_->setMaxLength(5);
    timeout_edit_->setValidator(new QIntValidator(0, 86400, timeout_edit_));
    timeout_edit_->setAccessibleName(QStringLiteral("Query read timeout in seconds"));
    tb->addWidget(timeout_edit_);
    tb->addWidget(new QLabel(QStringLiteral("0 = no timeout"), top_bar));

    connect(timeout_edit_, &QLineEdit::editingFinished, this, [this]() {
        bool ok = false;
        const int v = timeout_edit_->text().trimmed().toInt(&ok);
        if (ok && v >= 0 && v <= 86400) {
            query_options_.execution_timeout_seconds = v;
        }
    });

    auto make_tab_btn = [&](IconId icon, const QString& text, bool enabled) {
        auto* btn = new QToolButton(top_bar);
        btn->setIcon(get_icon(icon));
        btn->setIconSize(QSize(16, 16));
        btn->setText(text);
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setEnabled(enabled);
        btn->setAutoRaise(true);
        tb->addWidget(btn);
        return btn;
    };

    go_to_error_btn_  = make_tab_btn(IconId::GoToError, QStringLiteral("Go to Error"), false);
    est_plan_btn_     = make_tab_btn(IconId::PlanEstimated, QStringLiteral("Estimated Plan"), false);
    analyze_plan_btn_ = make_tab_btn(IconId::PlanActual, QStringLiteral("Analyze Plan…"), false);
    design_query_btn_ = make_tab_btn(IconId::DesignQuery, QStringLiteral("Design Query…"), true);
    connect(go_to_error_btn_, &QToolButton::clicked, this, &QueryTabWidget::navigate_to_error);
    connect(est_plan_btn_, &QToolButton::clicked, this, &QueryTabWidget::run_estimated_plan);
    connect(analyze_plan_btn_, &QToolButton::clicked, this, [this]() { run_analyze_plan(false); });
    tb->addStretch(1);

    root->addWidget(top_bar);

    // 2. Vertical Splitter (Top Pane: Inline Search Bars + SqlCodeEditor 3* / Bottom OutputPanel 2*)
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(QStringLiteral("QueryVerticalSplitter"));
    splitter_->setHandleWidth(theme::kSplitterWidthPx);
    splitter_->setChildrenCollapsible(false);

    auto* editor_container = new QWidget(splitter_);
    editor_container->setMinimumHeight(theme::kEditorMinHeightPx);
    auto* ed_layout = new QVBoxLayout(editor_container);
    ed_layout->setContentsMargins(0, 0, 0, 0);
    ed_layout->setSpacing(0);

    go_to_line_bar_   = new GoToLineBar(editor_container);
    find_replace_bar_ = new FindReplaceBar(editor_container);
    ed_layout->addWidget(go_to_line_bar_);
    ed_layout->addWidget(find_replace_bar_);

    editor_ = new SqlCodeEditor(editor_container);
    editor_->setMinimumHeight(theme::kEditorMinHeightPx);
    editor_->setAccessibleName(QStringLiteral("SQL editor"));
    editor_->installEventFilter(this);
    ed_layout->addWidget(editor_, 1);

    completion_popup_ = new CompletionPopupWidget(this);
    connect(completion_popup_, &CompletionPopupWidget::item_accepted,
            this, &QueryTabWidget::accept_completion_item);

    draft_checkpoint_timer_ = new QTimer(this);
    draft_checkpoint_timer_->setSingleShot(true);
    draft_checkpoint_timer_->setInterval(2000);
    connect(draft_checkpoint_timer_, &QTimer::timeout, this, [this]() {
        if (dirty_ && file_path_.isEmpty()) {
            emit dirty_checkpoint_requested();
        }
    });

    connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
        error_offset_.reset();
        go_to_error_btn_->setEnabled(false);
        if (plan_widget_ != nullptr && !plan_widget_->statements().empty()) {
            plan_widget_->set_stale(true);
        }
        if (!dirty_) {
            dirty_ = true;
            emit state_changed();
        }
        if (file_path_.isEmpty()) {
            draft_checkpoint_timer_->start();
        }
        if (completion_popup_ != nullptr && completion_popup_->is_open()) {
            update_open_completion_filter();
        }
    });
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, &QueryTabWidget::update_caret_status);
    splitter_->addWidget(editor_container);

    // 3. Output Panel (Top Tab Strip 25 px #EEEEEE + Stacked Views + Export Flyout)
    output_panel_ = new QWidget(splitter_);
    output_panel_->setObjectName(QStringLiteral("OutputPanel"));
    output_panel_->setAttribute(Qt::WA_StyledBackground, true);
    output_panel_->setMinimumHeight(theme::kResultsMinHeightPx);
    auto* out_layout = new QVBoxLayout(output_panel_);
    out_layout->setContentsMargins(0, 0, 0, 0);
    out_layout->setSpacing(0);

    output_strip_bar_ = new QWidget(output_panel_);
    output_strip_bar_->setObjectName(QStringLiteral("OutputTabStripBar"));
    output_strip_bar_->setAttribute(Qt::WA_StyledBackground, true);
    output_strip_bar_->setFixedHeight(theme::kOutputTabStripHeightPx);
    output_strip_bar_->setMinimumWidth(0);
    output_strip_bar_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* strip_layout = new QHBoxLayout(output_strip_bar_);
    strip_layout->setContentsMargins(2, 2, 6, 0);
    strip_layout->setSpacing(4);

    output_tab_bar_ = new QTabBar(output_strip_bar_);
    output_tab_bar_->setObjectName(QStringLiteral("OutputTabBar"));
    output_tab_bar_->setDrawBase(false);
    output_tab_bar_->setExpanding(false);
    output_tab_bar_->setUsesScrollButtons(true);
    output_tab_bar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    output_tab_bar_->setIconSize(QSize(theme::kIconSizePx, theme::kIconSizePx));
    output_tab_bar_->addTab(get_icon(IconId::TabResults), QStringLiteral("Results"));
    output_tab_bar_->addTab(get_icon(IconId::TabMessages), QStringLiteral("Messages"));
    output_tab_bar_->addTab(get_icon(IconId::TabPlan), QStringLiteral("Execution plan"));
    output_tab_bar_->addTab(get_icon(IconId::TabSession), QStringLiteral("Session"));
    output_tab_bar_->addTab(get_icon(IconId::TabStats), QStringLiteral("Client Statistics"));
    output_tab_bar_->setTabVisible(4, false);
    strip_layout->addWidget(output_tab_bar_);
    strip_layout->addStretch(1);

    cancel_query_btn_ = new QToolButton(output_strip_bar_);
    cancel_query_btn_->setObjectName(QStringLiteral("CancelQueryButton"));
    cancel_query_btn_->setText(QStringLiteral("Cancel Query"));
    cancel_query_btn_->setVisible(false);
    cancel_query_btn_->setEnabled(false);
    cancel_query_btn_->setAutoRaise(true);
    cancel_query_btn_->setAccessibleName(QStringLiteral("Cancel running query"));
    strip_layout->addWidget(cancel_query_btn_);
    connect(cancel_query_btn_, &QToolButton::clicked, this, &QueryTabWidget::cancel_query);

    cancel_export_btn_ = new QToolButton(output_strip_bar_);
    cancel_export_btn_->setObjectName(QStringLiteral("CancelExportButton"));
    cancel_export_btn_->setText(QStringLiteral("Cancel Export"));
    cancel_export_btn_->setVisible(false);
    cancel_export_btn_->setEnabled(false);
    cancel_export_btn_->setAutoRaise(true);
    cancel_export_btn_->setAccessibleName(QStringLiteral("Cancel running export"));
    strip_layout->addWidget(cancel_export_btn_);
    connect(cancel_export_btn_, &QToolButton::clicked, this, &QueryTabWidget::cancel_export);

    export_csv_btn_ = new QToolButton(output_strip_bar_);
    export_csv_btn_->setObjectName(QStringLiteral("ExportCsvButton"));
    export_csv_btn_->setText(QStringLiteral("Export CSV…"));
    export_csv_btn_->setEnabled(false);
    export_csv_btn_->setAutoRaise(true);
    strip_layout->addWidget(export_csv_btn_);
    connect(export_csv_btn_, &QToolButton::clicked, this, [this]() {
        core::ExportOptions opts;
        opts.format          = core::ExportFormat::Csv;
        opts.scope           = core::ExportScope::AllDisplayedRows;
        opts.csv_delimiter   = ',';
        opts.include_headers = true;
        opts.utf8_bom        = false;
        start_export(opts);
    });

    export_flyout_btn_ = new QToolButton(output_strip_bar_);
    export_flyout_btn_->setObjectName(QStringLiteral("ExportFlyoutButton"));
    export_flyout_btn_->setIcon(get_icon(IconId::ExportArrow));
    export_flyout_btn_->setIconSize(QSize(12, 12));
    export_flyout_btn_->setText(QStringLiteral("Exportar ▼"));
    export_flyout_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    export_flyout_btn_->setCheckable(true);
    export_flyout_btn_->setAutoRaise(true);
    export_flyout_btn_->setAccessibleName(QStringLiteral("Export options"));
    strip_layout->addWidget(export_flyout_btn_);
    connect(export_flyout_btn_, &QToolButton::toggled, this, &QueryTabWidget::toggle_export_flyout);

    out_layout->addWidget(output_strip_bar_);

    output_stack_ = new QStackedWidget(output_panel_);
    connect(output_tab_bar_, &QTabBar::currentChanged, output_stack_, &QStackedWidget::setCurrentIndex);

    // Page 0: Results (High-Performance ResultGridViewport + bottom ResultSetsStrip / Results-to-Text)
    results_stack_ = new QStackedWidget(output_stack_);
    results_grid_container_ = new QWidget(results_stack_);
    results_grid_container_->setObjectName(QStringLiteral("ResultsGridContainer"));
    auto* grid_container_layout = new QVBoxLayout(results_grid_container_);
    grid_container_layout->setContentsMargins(0, 0, 0, 0);
    grid_container_layout->setSpacing(0);

    result_grid_ = new ResultGridViewport(results_grid_container_);
    result_grid_->setAccessibleName(QStringLiteral("Query results grid"));
    grid_container_layout->addWidget(result_grid_, 1);

    result_sets_strip_ = new QTabBar(results_grid_container_);
    result_sets_strip_->setObjectName(QStringLiteral("ResultSetsStrip"));
    result_sets_strip_->setShape(QTabBar::RoundedSouth);
    result_sets_strip_->setDrawBase(false);
    result_sets_strip_->setExpanding(false);
    result_sets_strip_->setVisible(false);
    grid_container_layout->addWidget(result_sets_strip_);

    connect(result_sets_strip_, &QTabBar::currentChanged, this, &QueryTabWidget::switch_active_result_set);
    connect(result_grid_, &ResultGridViewport::json_link_clicked, this, &QueryTabWidget::open_json_document_requested);
    connect(result_grid_, &ResultGridViewport::status_message, this, &QueryTabWidget::status_hint);
    connect(result_grid_, &ResultGridViewport::selection_changed, this, &QueryTabWidget::update_export_availability);
    connect(result_grid_, &ResultGridViewport::save_results_requested, this, [this]() {
        core::ExportOptions opts = export_flyout_ ? export_flyout_->current_options() : core::ExportOptions{};
        if (!result_grid_->has_selection() && opts.scope == core::ExportScope::SelectedRows) {
            opts.scope = core::ExportScope::AllDisplayedRows;
        }
        start_export(opts);
    });

    results_stack_->addWidget(results_grid_container_);

    results_text_view_ = new QPlainTextEdit(results_stack_);
    results_text_view_->setObjectName(QStringLiteral("ResultsTextView"));
    results_text_view_->setReadOnly(true);
    results_text_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    results_text_view_->setFont(theme::resolve_mono_font(theme::kResultsTextFontSizePx));
    results_text_view_->setFrameShape(QFrame::NoFrame);
    results_stack_->addWidget(results_text_view_);
    output_stack_->addWidget(results_stack_);

    // Page 1: Messages
    messages_view_ = new QPlainTextEdit(output_stack_);
    messages_view_->setObjectName(QStringLiteral("MessagesView"));
    messages_view_->setReadOnly(true);
    messages_view_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    messages_view_->setFont(theme::resolve_mono_font(theme::kMessagesFontSizePx));
    messages_view_->setPlainText(QStringLiteral("Connect to a server to execute this query."));
    output_stack_->addWidget(messages_view_);

    // Page 2: Execution plan (Graphical ExecutionPlanWidget)
    plan_widget_ = new ExecutionPlanWidget(true, output_stack_);
    connect(plan_widget_, &ExecutionPlanWidget::show_in_new_tab_requested,
            this, &QueryTabWidget::show_plan_in_new_tab_requested);
    connect(plan_widget_, &ExecutionPlanWidget::open_script_in_new_query_requested,
            this, &QueryTabWidget::open_script_in_new_query_requested);
    connect(plan_widget_, &ExecutionPlanWidget::status_message,
            this, &QueryTabWidget::status_hint);
    output_stack_->addWidget(plan_widget_);

    // Page 3: Session
    auto* sess_page = new QWidget(output_stack_);
    auto* sess_lay = new QVBoxLayout(sess_page);
    sess_lay->setContentsMargins(8, 8, 8, 8);
    auto* sess_lbl = new QLabel(
        QStringLiteral("Inspect the connection owned by this query. No passwords are displayed."),
        sess_page);
    sess_lbl->setWordWrap(true);
    sess_lay->addWidget(sess_lbl, 0, Qt::AlignTop);
    output_stack_->addWidget(sess_page);

    // Page 4: Client Statistics
    auto* stats_page = new QWidget(output_stack_);
    auto* stats_lay = new QVBoxLayout(stats_page);
    stats_lay->setContentsMargins(8, 8, 8, 8);
    stats_lay->addWidget(new QLabel(QStringLiteral("Client statistics for this query session."), stats_page), 0, Qt::AlignTop);
    output_stack_->addWidget(stats_page);

    out_layout->addWidget(output_stack_, 1);

    // Export Options Flyout overlay anchored inside output_panel_
    export_flyout_ = new ExportFlyoutWidget(output_panel_);
    export_flyout_->setVisible(false);
    connect(export_flyout_, &ExportFlyoutWidget::size_hint_changed, this, &QueryTabWidget::update_flyout_position);
    connect(export_flyout_, &ExportFlyoutWidget::export_requested, this, [this](const core::ExportOptions& opts) {
        export_flyout_btn_->setChecked(false);
        start_export(opts);
    });

    splitter_->addWidget(output_panel_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({360, 240});
    root->addWidget(splitter_, 1);

    // 4. Query Status Bar (24 px, #FFF49C / #FDE7E9, 6 right segments)
    status_bar_ = new QWidget(this);
    status_bar_->setObjectName(QStringLiteral("QueryStatusBar"));
    status_bar_->setAttribute(Qt::WA_StyledBackground, true);
    status_bar_->setProperty("errorState", false);
    status_bar_->setFixedHeight(theme::kQueryStatusBarHeightPx);
    status_bar_->setMinimumWidth(0);
    status_bar_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    const QFont status_font = theme::resolve_ui_font(theme::kQueryStatusFontSizePx);
    auto* sb_layout = new QHBoxLayout(status_bar_);
    sb_layout->setContentsMargins(6, 0, 0, 0);
    sb_layout->setSpacing(6);

    status_icon_ = new QLabel(status_bar_);
    status_icon_->setFixedSize(theme::kStatusIconSizePx, theme::kStatusIconSizePx);
    sb_layout->addWidget(status_icon_);

    status_message_ = new QLabel(status_bar_);
    status_message_->setObjectName(QStringLiteral("QueryStatusMessage"));
    status_message_->setFont(status_font);
    status_message_->setMinimumWidth(0);
    status_message_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sb_layout->addWidget(status_message_, 1);

    auto make_segment = [&](const QString& initial_text) {
        auto* lbl = new QLabel(initial_text, status_bar_);
        lbl->setObjectName(QStringLiteral("QueryStatusSegment"));
        lbl->setFont(status_font);
        lbl->setMinimumWidth(0);
        lbl->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        sb_layout->addWidget(lbl);
        return lbl;
    };

    status_server_   = make_segment(QStringLiteral("localhost (PostgreSQL)"));
    status_user_     = make_segment(QStringLiteral("-"));
    status_database_ = make_segment(QStringLiteral("Not connected"));
    status_elapsed_  = make_segment(QStringLiteral("00:00:00"));
    status_caret_    = make_segment(QStringLiteral("Linha: 1, Col: 1"));
    status_rows_     = make_segment(QStringLiteral("0 linhas"));

    root->addWidget(status_bar_);

    elapsed_timer_ = new QTimer(this);
    elapsed_timer_->setInterval(200);
    connect(elapsed_timer_, &QTimer::timeout, this, &QueryTabWidget::update_elapsed_clock);
}

void QueryTabWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (export_flyout_ != nullptr && export_flyout_->isVisible()) {
        update_flyout_position();
    }
}

bool QueryTabWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == editor_ && event != nullptr && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const int key = ke->key();
        const Qt::KeyboardModifiers mods = ke->modifiers() &
            (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);

        if (completion_popup_ != nullptr && completion_popup_->is_open()) {
            if (key == Qt::Key_Up && mods == Qt::NoModifier) {
                completion_popup_->move_selection(-1);
                return true;
            }
            if (key == Qt::Key_Down && mods == Qt::NoModifier) {
                completion_popup_->move_selection(1);
                return true;
            }
            if (key == Qt::Key_PageUp && mods == Qt::NoModifier) {
                completion_popup_->move_selection(-8);
                return true;
            }
            if (key == Qt::Key_PageDown && mods == Qt::NoModifier) {
                completion_popup_->move_selection(8);
                return true;
            }
            if (key == Qt::Key_Tab && mods == Qt::NoModifier) {
                if (const auto item = completion_popup_->current_item()) {
                    accept_completion_item(
                        QString::fromStdString(item->text),
                        completion_popup_->replace_start_char_pos());
                    return true;
                }
            }
            if ((key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::NoModifier) {
                if (completion_popup_->has_explicit_choice()) {
                    if (const auto item = completion_popup_->current_item()) {
                        accept_completion_item(
                            QString::fromStdString(item->text),
                            completion_popup_->replace_start_char_pos());
                        return true;
                    }
                }
                completion_popup_->close_popup();
                return false; // Let Enter insert a normal newline when no item was navigated.
            }
            if (key == Qt::Key_Escape && mods == Qt::NoModifier) {
                completion_popup_->close_popup();
                return true;
            }
        }

        if (mods == Qt::ControlModifier && (key == Qt::Key_Space || key == Qt::Key_J)) {
            trigger_intellisense(false);
            return true;
        }
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_R) {
            refresh_intellisense_cache();
            return true;
        }
        if (intellisense_enabled_ && ke->text() == QStringLiteral(".") &&
            (mods == Qt::NoModifier || mods == Qt::ShiftModifier)) {
            QTimer::singleShot(0, this, [this]() {
                trigger_intellisense(true);
            });
            return false;
        }

        if (key == Qt::Key_Escape && mods == Qt::NoModifier) {
            if (handle_escape_key()) {
                return true;
            }
        } else if (key == Qt::Key_F3 && mods == Qt::NoModifier && find_replace_bar_->isVisible()) {
            find_next();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QueryTabWidget::set_dirty(bool dirty) {
    if (dirty_ == dirty) {
        return;
    }
    dirty_ = dirty;
    if (!dirty_ && draft_checkpoint_timer_ != nullptr) {
        draft_checkpoint_timer_->stop();
    }
    emit state_changed();
}

void QueryTabWidget::set_query_options(const core::QueryOptions& opts) {
    query_options_ = opts.normalized();
    if (timeout_edit_ != nullptr && !busy_) {
        timeout_edit_->setText(QString::number(query_options_.execution_timeout_seconds));
    }
    if (result_grid_ != nullptr) {
        result_grid_->set_display_json_as_link(query_options_.display_json_as_link);
        result_grid_->set_retain_line_breaks_on_copy(query_options_.retain_line_breaks);
    }
    emit state_changed();
}

void QueryTabWidget::set_results_destination(capybaradb::core::ResultsDestination dest) {
    if (results_dest_ == dest) {
        return;
    }
    results_dest_ = dest;
    update_destination_ui();
    emit state_changed();
}

void QueryTabWidget::set_include_actual_plan(bool enabled) {
    if (include_actual_plan_ == enabled) {
        return;
    }
    include_actual_plan_ = enabled;
    emit state_changed();
}

void QueryTabWidget::set_include_client_statistics(bool enabled) {
    if (include_client_stats_ == enabled) {
        return;
    }
    include_client_stats_ = enabled;
    output_tab_bar_->setTabVisible(4, enabled);
    emit state_changed();
}

void QueryTabWidget::set_intellisense_enabled(bool enabled) {
    if (intellisense_enabled_ == enabled) {
        return;
    }
    intellisense_enabled_ = enabled;
    if (!intellisense_enabled_ && completion_popup_ != nullptr) {
        completion_popup_->close_popup();
    }
    emit state_changed();
}

bool QueryTabWidget::word_wrap() const {
    return editor_->lineWrapMode() != QPlainTextEdit::NoWrap;
}

void QueryTabWidget::set_word_wrap(bool enabled) {
    editor_->setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

bool QueryTabWidget::show_white_space() const {
    return (editor_->document()->defaultTextOption().flags() & QTextOption::ShowTabsAndSpaces) != 0;
}

void QueryTabWidget::set_show_white_space(bool enabled) {
    QTextOption opt = editor_->document()->defaultTextOption();
    QTextOption::Flags flags = opt.flags();
    if (enabled) {
        flags |= QTextOption::ShowTabsAndSpaces;
    } else {
        flags &= ~QTextOption::ShowTabsAndSpaces;
    }
    opt.setFlags(flags);
    editor_->document()->setDefaultTextOption(opt);
}

bool QueryTabWidget::is_results_pane_visible() const {
    return output_panel_->isVisible();
}

void QueryTabWidget::set_results_pane_visible(bool visible) {
    output_panel_->setVisible(visible);
    emit state_changed();
}

void QueryTabWidget::toggle_results_pane() {
    set_results_pane_visible(!is_results_pane_visible());
}

QString QueryTabWidget::editor_text() const {
    return editor_->toPlainText();
}

void QueryTabWidget::set_editor_text(const QString& text) {
    editor_->setPlainText(text);
}

void QueryTabWidget::focus_editor() {
    editor_->setFocus(Qt::ShortcutFocusReason);
}

void QueryTabWidget::show_find_bar() {
    go_to_line_bar_->setVisible(false);
    find_replace_bar_->open_find(editor_);
}

void QueryTabWidget::show_replace_bar() {
    go_to_line_bar_->setVisible(false);
    find_replace_bar_->open_replace(editor_);
}

void QueryTabWidget::show_go_to_line_bar() {
    find_replace_bar_->setVisible(false);
    go_to_line_bar_->open_for_editor(editor_);
}

void QueryTabWidget::find_next() {
    if (!find_replace_bar_->isVisible()) {
        find_replace_bar_->open_find(editor_);
        return;
    }
    (void)find_replace_bar_->find_next();
}

bool QueryTabWidget::handle_escape_key() {
    if (completion_popup_ != nullptr && completion_popup_->is_open()) {
        completion_popup_->close_popup();
        return true;
    }
    if (export_flyout_ != nullptr && export_flyout_->isVisible()) {
        export_flyout_btn_->setChecked(false);
        return true;
    }
    if (plan_widget_ != nullptr && plan_widget_->is_find_bar_visible()) {
        plan_widget_->close_find_bar();
        return true;
    }
    if (go_to_line_bar_->isVisible()) {
        go_to_line_bar_->close_bar();
        return true;
    }
    if (find_replace_bar_->isVisible()) {
        find_replace_bar_->close_bar();
        return true;
    }
    return false;
}

void QueryTabWidget::comment_selection(bool uncomment) {
    const auto res = editor_->comment_selection(uncomment);
    if (!res.has_value()) {
        set_status_state(StatusKind::Idle, res.error(), false);
        emit status_hint(res.error());
        editor_->setFocus(Qt::ShortcutFocusReason);
        return;
    }
    const int changed = *res;
    const QString msg = (changed == 0)
        ? QStringLiteral("No leading line comments to remove.")
        : QStringLiteral("%1 %2 line(s).")
              .arg(uncomment ? QStringLiteral("Uncommented") : QStringLiteral("Commented"))
              .arg(changed);
    set_status_state(StatusKind::Idle, msg, false);
    emit status_hint(msg);
    editor_->setFocus(Qt::ShortcutFocusReason);
}

void QueryTabWidget::set_status_state(StatusKind kind, const QString& message, bool error_bg) {
    status_message_->setText(message);
    status_bar_->setProperty("errorState", error_bg);
    status_bar_->style()->unpolish(status_bar_);
    status_bar_->style()->polish(status_bar_);

    IconId icon_id = IconId::StatusIdle;
    switch (kind) {
    case StatusKind::Idle:
        icon_id = IconId::StatusIdle;
        break;
    case StatusKind::Running:
        icon_id = IconId::StatusRunning;
        break;
    case StatusKind::Success:
        icon_id = IconId::StatusSuccess;
        break;
    case StatusKind::Error:
    case StatusKind::Canceled:
        icon_id = IconId::StatusError;
        break;
    }
    const qreal dpr = devicePixelRatioF();
    status_icon_->setPixmap(render_icon_pixmap(icon_id, theme::kStatusIconSizePx, dpr));
}

void QueryTabWidget::update_caret_status() {
    const QTextCursor cursor = editor_->textCursor();
    const int line = cursor.blockNumber() + 1;
    const int col  = cursor.positionInBlock() + 1;
    status_caret_->setText(QStringLiteral("Linha: %1, Col: %2").arg(line).arg(col));
}

void QueryTabWidget::update_elapsed_clock() {
    const int64_t secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - exec_started_).count();
    status_elapsed_->setText(format_hhmmss(secs));
}

void QueryTabWidget::update_row_count_segment(uint64_t rows, bool partial) {
    status_rows_->setText(
        partial ? QStringLiteral("%1 linhas (parcial)").arg(rows)
                : QStringLiteral("%1 linhas").arg(rows));
}

void QueryTabWidget::update_destination_ui() {
    const bool is_text = (results_dest_ == capybaradb::core::ResultsDestination::Text);
    const bool is_file = (results_dest_ == capybaradb::core::ResultsDestination::File);

    results_stack_->setCurrentIndex(is_text ? 1 : 0);
    output_tab_bar_->setTabVisible(0, !is_file);
    output_tab_bar_->setTabVisible(1, !is_text);
    if (is_file) {
        output_tab_bar_->setCurrentIndex(1);
    } else if (is_text) {
        output_tab_bar_->setCurrentIndex(0);
    }
}

void QueryTabWidget::update_action_buttons() {
    const bool can_run = connected_ && !busy_;
    if (est_plan_btn_ != nullptr) {
        est_plan_btn_->setEnabled(can_run);
    }
    if (analyze_plan_btn_ != nullptr) {
        analyze_plan_btn_->setEnabled(can_run);
    }
}

void QueryTabWidget::update_export_availability() {
    const core::ResultSetStorage* rs = nullptr;
    if (current_result_ && active_result_index_ >= 0) {
        const uint32_t rs_count = current_result_->result_set_count.load(std::memory_order_acquire);
        if (static_cast<uint32_t>(active_result_index_) < rs_count) {
            rs = current_result_->result_sets[static_cast<size_t>(active_result_index_)].load(std::memory_order_acquire);
        }
    }
    const bool has_result = (!busy_ && rs != nullptr && rs->col_count > 0);
    const bool has_sel    = (has_result && result_grid_ != nullptr && result_grid_->has_selection());

    if (export_csv_btn_ != nullptr) {
        export_csv_btn_->setEnabled(has_result && !exporting_);
    }
    if (export_flyout_ != nullptr) {
        export_flyout_->set_availability(has_result, has_sel, exporting_);
    }
    if (cancel_export_btn_ != nullptr) {
        cancel_export_btn_->setVisible(exporting_);
        cancel_export_btn_->setEnabled(exporting_ && export_cancel_ && !export_cancel_->is_canceled());
    }
}

void QueryTabWidget::toggle_export_flyout(bool open) {
    if (open) {
        update_export_availability();
        export_flyout_->adjustSize();
        update_flyout_position();
        export_flyout_->raise();
    }
    export_flyout_->setVisible(open);
}

void QueryTabWidget::update_flyout_position() {
    if (export_flyout_ == nullptr || output_panel_ == nullptr) {
        return;
    }
    const QSize sz = export_flyout_->sizeHint();
    const int x = std::max(4, output_panel_->width() - sz.width() - 6);
    const int y = theme::kOutputTabStripHeightPx;
    export_flyout_->setGeometry(x, y, sz.width(), sz.height());
}

void QueryTabWidget::set_driver_factory(
    std::function<std::unique_ptr<drivers::DbDriver>(drivers::EngineKind)> factory) {
    if (factory) {
        driver_factory_ = std::move(factory);
    }
}

void QueryTabWidget::set_catalog_provider_factory(
    std::function<std::unique_ptr<drivers::CatalogProvider>(drivers::EngineKind)> factory) {
    if (factory) {
        catalog_provider_factory_ = std::move(factory);
    }
}

void QueryTabWidget::set_completion_catalog_for_test(core::SqlCompletionCatalog catalog) {
    completion_catalog_       = std::move(catalog);
    has_completion_catalog_   = true;
    completion_cache_key_     = current_completion_cache_key();
    completion_test_override_ = true;
}

core::CatalogWorkerPool& QueryTabWidget::effective_worker_pool() {
    if (worker_pool_ != nullptr) {
        return *worker_pool_;
    }
    if (!fallback_pool_) {
        fallback_pool_ = std::make_unique<core::CatalogWorkerPool>(2);
    }
    return *fallback_pool_;
}

std::string QueryTabWidget::current_completion_cache_key() const {
    if (!connection_config_.has_value()) {
        return {};
    }
    const auto& c = *connection_config_;
    return engine_to_history_name(c.engine) + "|" + c.host + "|" + std::to_string(c.port) + "|" +
           c.database + "|" + c.username;
}

void QueryTabWidget::update_connection_segments() {
    if (!connected_) {
        status_server_->setText(QStringLiteral("(No server)"));
        status_user_->setText(QStringLiteral("-"));
        status_database_->setText(QStringLiteral("Not connected"));
        return;
    }

    const QString host_part = (
        (session_info_.engine == drivers::EngineKind::PostgreSql && session_info_.port == 5432) ||
        (session_info_.engine == drivers::EngineKind::SqlServer && session_info_.port == 1433))
        ? QString::fromStdString(session_info_.host)
        : QStringLiteral("%1:%2").arg(QString::fromStdString(session_info_.host)).arg(session_info_.port);

    QString ver_short = session_info_.engine == drivers::EngineKind::SqlServer
        ? QStringLiteral("SQL Server")
        : QStringLiteral("PostgreSQL");
    if (!session_info_.server_version.empty()) {
        const QString full = QString::fromStdString(session_info_.server_version).trimmed();
        if (session_info_.engine == drivers::EngineKind::PostgreSql) {
            const QStringList parts = full.split(u' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                ver_short = parts[0] + u' ' + parts[1];
            }
        } else {
            const int nl = full.indexOf(u'\n');
            ver_short = (nl > 0 ? full.left(nl) : full).trimmed();
            if (ver_short.size() > 28) {
                ver_short = ver_short.left(25) + QStringLiteral("…");
            }
        }
    }

    status_server_->setText(QStringLiteral("%1 (%2)").arg(host_part, ver_short));
    const QString id_label = (session_info_.engine == drivers::EngineKind::SqlServer)
        ? QStringLiteral("SPID")
        : QStringLiteral("PID");
    status_user_->setText(QStringLiteral("%1 (%2: %3)")
        .arg(QString::fromStdString(session_info_.username), id_label)
        .arg(session_info_.session_id));
    status_database_->setText(QString::fromStdString(session_info_.database));
}

void QueryTabWidget::set_connected_state(
    const drivers::ConnectionConfig& cfg,
    const drivers::SessionInfo& info) {
    connection_config_ = cfg;
    session_info_      = info;
    connected_         = true;
    editor_->set_engine_dialect(cfg.engine);
    if (!completion_test_override_) {
        completion_catalog_ = {};
        has_completion_catalog_ = false;
        completion_cache_key_.clear();
    }
    update_connection_segments();
    update_action_buttons();
    set_status_state(StatusKind::Idle, QStringLiteral("Pronto"), false);
    if (messages_view_->toPlainText() == QStringLiteral("Connect to a server to execute this query.")) {
        messages_view_->setPlainText(
            QStringLiteral("Connected to %1. Each query tab has its own session.")
                .arg(QString::fromStdString(info.database)));
    }
    emit connection_changed(true);
    emit state_changed();
}

void QueryTabWidget::connect_async(
    const drivers::ConnectionConfig& cfg,
    core::CatalogWorkerPool* pool,
    std::function<void(bool ok, const QString& err)> on_finished) {
    if (pool != nullptr) {
        worker_pool_ = pool;
    }
    session_worker_.cancel_active();
    const uint64_t gen = ++op_generation_;
    set_status_state(StatusKind::Running, QStringLiteral("Conectando…"), false);

    QPointer<QueryTabWidget> self(this);
    auto factory = driver_factory_;

    (void)session_worker_.submit([self, cfg, gen, factory, on_finished = std::move(on_finished)](
                                      core::DbSessionWorker& worker,
                                      core::CancelToken& cancel) mutable {
        std::unique_ptr<drivers::DbDriver> candidate = factory(cfg.engine);
        auto res = candidate->connect(cfg, cancel);
        drivers::SessionInfo info;
        if (res.has_value()) {
            info = candidate->session_info();
            if (auto old = worker.replace_driver(std::move(candidate))) {
                old->disconnect();
            }
        } else {
            candidate->disconnect();
        }

        QMetaObject::invokeMethod(
            self,
            [self, cfg, info = std::move(info), res = std::move(res), gen, on_finished = std::move(on_finished)]() mutable {
                if (!self || gen != self->op_generation_) {
                    return;
                }
                if (!res.has_value()) {
                    const QString err_msg = QString::fromStdString(res.error().format_short());
                    self->set_status_state(StatusKind::Error, err_msg, true);
                    emit self->status_hint(err_msg);
                    if (on_finished) {
                        on_finished(false, err_msg);
                    }
                    return;
                }

                self->set_connected_state(cfg, info);
                if (on_finished) {
                    on_finished(true, QString{});
                }
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::disconnect_session() {
    session_worker_.cancel_active();
    if (completion_load_cancel_) {
        completion_load_cancel_->cancel();
    }
    ++op_generation_;
    if (elapsed_timer_ != nullptr) {
        elapsed_timer_->stop();
    }
    cancel_query_btn_->setVisible(false);
    cancel_query_btn_->setEnabled(false);

    (void)session_worker_.submit([](core::DbSessionWorker& worker, core::CancelToken&) {
        if (auto old = worker.replace_driver(nullptr)) {
            old->disconnect();
        }
    });

    connection_config_.reset();
    session_info_ = {};
    connected_    = false;
    busy_         = false;
    canceling_    = false;
    if (!completion_test_override_) {
        completion_catalog_ = {};
        has_completion_catalog_ = false;
        completion_cache_key_.clear();
    }
    if (completion_popup_ != nullptr) {
        completion_popup_->close_popup();
    }
    update_connection_segments();
    update_action_buttons();
    update_export_availability();
    set_status_state(StatusKind::Idle, QStringLiteral("Não conectado"), false);
    emit connection_changed(false);
    emit state_changed();
}

void QueryTabWidget::switch_database_async(
    const QString& new_database,
    core::CatalogWorkerPool* pool,
    std::function<bool(const QString& warning)> confirm_loss_risk) {
    if (pool != nullptr) {
        worker_pool_ = pool;
    }
    if (!connected_ || busy_ || new_database.trimmed().isEmpty()) {
        return;
    }
    if (new_database.toStdString() == session_info_.database) {
        return;
    }

    const uint64_t gen = ++op_generation_;
    const std::string target_db = new_database.trimmed().toStdString();
    const drivers::EngineKind engine = session_info_.engine;

    QPointer<QueryTabWidget> self(this);
    set_status_state(StatusKind::Running, QStringLiteral("Changing to %1…").arg(new_database), false);

    (void)session_worker_.submit([self, gen, target_db, engine, confirm_loss_risk = std::move(confirm_loss_risk)](
                                      core::DbSessionWorker& worker,
                                      core::CancelToken& cancel) mutable {
        drivers::DbDriver* drv = worker.driver();
        if (drv == nullptr || !drv->is_connected()) {
            return;
        }
        auto risk_res = drv->inspect_loss_risk(cancel);

        QMetaObject::invokeMethod(
            self,
            [self, gen, target_db, engine, risk_res = std::move(risk_res), confirm_loss_risk = std::move(confirm_loss_risk)]() mutable {
                if (!self || gen != self->op_generation_) {
                    return;
                }
                if (risk_res.has_value() && risk_res->has_any_risk()) {
                    QString held;
                    if (risk_res->open_transaction && risk_res->temporary_tables) {
                        held = QStringLiteral("an open transaction and temporary tables");
                    } else if (risk_res->open_transaction) {
                        held = QStringLiteral("an open transaction");
                    } else {
                        held = QStringLiteral("temporary tables");
                    }
                    const QString reason = (engine == drivers::EngineKind::SqlServer)
                        ? QStringLiteral("Changing database updates this session context.")
                        : QStringLiteral("PostgreSQL changes database by opening a new connection, so moving to \"%1\" closes this session.")
                              .arg(QString::fromStdString(target_db));
                    const QString loss = risk_res->open_transaction
                        ? QStringLiteral("Uncommitted work is rolled back, and temporary tables and SET values are lost.")
                        : QStringLiteral("Temporary tables and SET values are lost.");
                    const QString warning = QStringLiteral("This query's session has %1.\n\n%2 %3\n\nChange database?")
                        .arg(held, reason, loss);

                    const bool proceed = confirm_loss_risk
                        ? confirm_loss_risk(warning)
                        : (QMessageBox::warning(
                               self,
                               QStringLiteral("CapybaraDB"),
                               warning,
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) == QMessageBox::Yes);
                    if (!proceed) {
                        self->set_status_state(StatusKind::Idle, QStringLiteral("Database not changed."), false);
                        emit self->connection_changed(true);
                        emit self->state_changed();
                        return;
                    }
                }

                (void)self->session_worker_.submit([self, gen, target_db](
                                                       core::DbSessionWorker& w2,
                                                       core::CancelToken& c2) {
                    drivers::DbDriver* d2 = w2.driver();
                    std::expected<void, drivers::DbError> sw = d2
                        ? d2->switch_database(target_db, c2)
                        : std::unexpected(drivers::DbError{.message = "Not connected."});
                    drivers::SessionInfo updated_info = (d2 && sw.has_value()) ? d2->session_info() : drivers::SessionInfo{};

                    QMetaObject::invokeMethod(
                        self,
                        [self, sw = std::move(sw), updated_info = std::move(updated_info), gen]() {
                            if (!self || gen != self->op_generation_) {
                                return;
                            }
                            if (!sw.has_value()) {
                                const QString err_msg = QString::fromStdString(sw.error().format_short());
                                self->set_status_state(StatusKind::Error, err_msg, true);
                                emit self->connection_changed(true);
                                emit self->state_changed();
                                return;
                            }
                            self->session_info_ = updated_info;
                            if (self->connection_config_.has_value()) {
                                self->connection_config_->database = self->session_info_.database;
                            }
                            if (!self->completion_test_override_) {
                                self->completion_catalog_ = {};
                                self->has_completion_catalog_ = false;
                                self->completion_cache_key_.clear();
                            }
                            self->update_connection_segments();
                            self->set_status_state(
                                StatusKind::Idle,
                                QStringLiteral("Connected to %1.").arg(QString::fromStdString(self->session_info_.database)),
                                false);
                            emit self->connection_changed(true);
                            emit self->state_changed();
                        },
                        Qt::QueuedConnection);
                });
            },
            Qt::QueuedConnection);
    });
}

QString QueryTabWidget::current_submitted_sql(int& selection_start_out) const {
    const QTextCursor cur = editor_->textCursor();
    if (cur.hasSelection()) {
        selection_start_out = cur.selectionStart();
        return cur.selectedText().replace(QChar::ParagraphSeparator, u'\n');
    }
    selection_start_out = 0;
    return editor_->toPlainText();
}

void QueryTabWidget::set_error_location(int offset, int select_len, uint64_t revision) {
    if (revision != editor_->edit_revision()) {
        return;
    }
    const int max_pos = std::max(0, editor_->document()->characterCount() - 1);
    if (offset < 0 || offset > max_pos) {
        return;
    }
    error_offset_     = offset;
    error_select_len_ = std::max(0, select_len);
    error_revision_   = revision;
    go_to_error_btn_->setEnabled(true);
}

void QueryTabWidget::navigate_to_error() {
    if (!error_offset_.has_value() || error_revision_ != editor_->edit_revision()) {
        go_to_error_btn_->setEnabled(false);
        return;
    }
    (void)editor_->navigate_to_char_offset(*error_offset_, error_select_len_);
}

bool QueryTabWidget::validate_before_send(
    const QString& sql,
    int selection_start,
    uint64_t revision,
    bool is_parse,
    uint32_t& timeout_out) {
    bool ok = false;
    const int parsed = timeout_edit_->text().trimmed().toInt(&ok);
    if (!ok || parsed < 0 || parsed > 86400) {
        const QString msg = QStringLiteral("Enter a read timeout from 0 to 86400 seconds (0 = no timeout).");
        set_status_state(StatusKind::Error, msg, true);
        timeout_edit_->setFocus();
        timeout_edit_->selectAll();
        return false;
    }
    timeout_out = static_cast<uint32_t>(parsed);
    query_options_.execution_timeout_seconds = parsed;

    if (session_info_.engine == drivers::EngineKind::PostgreSql) {
        const std::string utf8 = sql.toStdString();
        if (const auto unbound = dialects::find_postgres_unbound(utf8)) {
            set_error_location(
                selection_start + static_cast<int>(unbound->char_offset),
                1 + QString::number(unbound->first_number).size(),
                revision);
            const QString desc = QString::fromStdString(unbound->description());
            if (is_parse) {
                messages_view_->setPlainText(
                    QStringLiteral("Not parsed: the SQL contains parameter placeholder %1, which has no value.").arg(desc));
            } else {
                messages_view_->setPlainText(
                    QStringLiteral("Not sent: the SQL contains parameter placeholder %1, which has no value. "
                                   "Placeholders like $1 come from prepared statements or pg_stat_statements; "
                                   "replace each one with a literal value before executing.\n"
                                   "Nothing was sent to the server and the session stays open. "
                                   "Go to Error selects the first placeholder.")
                        .arg(desc));
            }
            set_status_state(
                StatusKind::Error,
                QStringLiteral("Parâmetro não vinculado $%1").arg(unbound->first_number),
                true);
            if (results_dest_ == capybaradb::core::ResultsDestination::File) {
                output_tab_bar_->setCurrentIndex(1);
            } else if (results_dest_ == capybaradb::core::ResultsDestination::Grid) {
                output_tab_bar_->setCurrentIndex(1);
            } else {
                results_text_view_->setPlainText(messages_view_->toPlainText());
            }
            return false;
        }
    }
    return true;
}

void QueryTabWidget::sync_result_sets_ui(const core::QueryExecutionResult& result) {
    const uint32_t rs_count = result.result_set_count.load(std::memory_order_acquire);
    while (result_view_states_.size() < rs_count) {
        const size_t idx = result_view_states_.size();
        result_view_states_.push_back(std::make_unique<grid::GridViewState>());
        const bool old_blk = result_sets_strip_->blockSignals(true);
        result_sets_strip_->addTab(QStringLiteral("Result %1").arg(idx + 1));
        result_sets_strip_->blockSignals(old_blk);
    }

    result_sets_strip_->setVisible(rs_count > 1);

    if (rs_count == 0) {
        active_result_index_ = -1;
        result_grid_->clear();
        update_export_availability();
        return;
    }

    if (active_result_index_ < 0 || static_cast<uint32_t>(active_result_index_) >= rs_count) {
        active_result_index_ = 0;
        const bool old_blk = result_sets_strip_->blockSignals(true);
        result_sets_strip_->setCurrentIndex(0);
        result_sets_strip_->blockSignals(old_blk);
        const core::ResultSetStorage* rs = result.result_sets[0].load(std::memory_order_acquire);
        result_grid_->set_active_result_set(rs, result_view_states_[0].get());
    } else {
        const core::ResultSetStorage* rs =
            result.result_sets[static_cast<size_t>(active_result_index_)].load(std::memory_order_acquire);
        if (result_grid_->storage() != rs) {
            result_grid_->set_active_result_set(rs, result_view_states_[static_cast<size_t>(active_result_index_)].get());
        } else {
            result_grid_->notify_rows_available();
        }
    }
    update_export_availability();
}

void QueryTabWidget::switch_active_result_set(int index) {
    if (!current_result_ || index < 0 || static_cast<size_t>(index) >= result_view_states_.size()) {
        return;
    }
    const uint32_t rs_count = current_result_->result_set_count.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(index) >= rs_count) {
        return;
    }
    active_result_index_ = index;
    const core::ResultSetStorage* rs =
        current_result_->result_sets[static_cast<size_t>(index)].load(std::memory_order_acquire);
    result_grid_->set_active_result_set(rs, result_view_states_[static_cast<size_t>(index)].get());
    update_export_availability();
}

void QueryTabWidget::on_stream_progress(
    const std::shared_ptr<core::QueryExecutionResult>& result,
    uint64_t gen) {
    active_sink_.clear_pending();
    if (!result || gen != op_generation_ || !busy_) {
        return;
    }

    const uint64_t rows = result->total_rows_read.load(std::memory_order_acquire);
    update_row_count_segment(rows, result->any_limit_hit());

    if (results_dest_ == capybaradb::core::ResultsDestination::Grid) {
        sync_result_sets_ui(*result);
        if (result->result_set_count.load(std::memory_order_acquire) > 0 && output_tab_bar_->currentIndex() != 0) {
            output_tab_bar_->setCurrentIndex(0);
        }
    }

    const auto msgs = result->messages_snapshot();
    if (!msgs.empty() && results_dest_ != capybaradb::core::ResultsDestination::Text) {
        QString live_msg;
        for (const core::MessageEntry& m : msgs) {
            if (!live_msg.isEmpty()) {
                live_msg += QLatin1Char('\n');
            }
            if (!m.sql_state.empty()) {
                const char* sev = (m.severity == core::MessageSeverity::Error) ? "ERROR"
                    : (m.severity == core::MessageSeverity::Warning) ? "WARNING"
                    : (m.severity == core::MessageSeverity::Notice) ? "NOTICE"
                    : "INFO";
                live_msg += QString::fromLatin1(sev) + QLatin1Char(' ') +
                            QString::fromUtf8(m.sql_state.data(), static_cast<qsizetype>(m.sql_state.size())) +
                            QStringLiteral(": ") +
                            QString::fromUtf8(m.text.data(), static_cast<qsizetype>(m.text.size()));
            } else {
                live_msg += QString::fromUtf8(m.text.data(), static_cast<qsizetype>(m.text.size()));
            }
        }
        messages_view_->setPlainText(live_msg);
    }
}

void QueryTabWidget::execute_query() {
    if (busy_ || !connected_) {
        return;
    }

    int submitted_start = 0;
    const QString sql = current_submitted_sql(submitted_start);
    const uint64_t submitted_rev = editor_->edit_revision();
    if (sql.trimmed().isEmpty()) {
        return;
    }

    uint32_t timeout_sec = 0;
    if (!validate_before_send(sql, submitted_start, submitted_rev, false, timeout_sec)) {
        return;
    }

    // Results to File (Ctrl+Shift+F): prompt for .rpt destination BEFORE sending to server.
    std::filesystem::path rpt_dest_path;
    QString rpt_dest_qstr;
    if (results_dest_ == core::ResultsDestination::File) {
        if (results_file_picker_) {
            rpt_dest_qstr = results_file_picker_();
        } else {
            rpt_dest_qstr = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("Save Results"),
                QStringLiteral("results.rpt"),
                QStringLiteral("Report Files (*.rpt);;Text Files (*.txt);;All Files (*.*)"));
        }
        if (rpt_dest_qstr.trimmed().isEmpty()) {
            const QString msg = QStringLiteral("No results file chosen.");
            set_status_state(StatusKind::Idle, msg, false);
            emit status_hint(msg);
            return;
        }
        rpt_dest_path = std::filesystem::path(rpt_dest_qstr.toStdWString());
    }

    error_offset_.reset();
    go_to_error_btn_->setEnabled(false);
    if (completion_popup_ != nullptr) {
        completion_popup_->close_popup();
    }

    const core::QueryOptions tab_opts = query_options_.normalized();
    const core::TextResultsFormat text_fmt = tab_opts.text_layout();
    const core::ResultsDestination exec_dest = results_dest_;

    drivers::ExecOptions opts;
    opts.command_timeout_sec = timeout_sec;
    opts.max_arena_bytes     = static_cast<size_t>(tab_opts.max_arena_mib) * 1024ull * 1024ull;
    opts.discard_results     = (exec_dest == core::ResultsDestination::Text)
                                   ? tab_opts.text_discard_results
                                   : ((exec_dest == core::ResultsDestination::Grid) ? tab_opts.grid_discard_results : false);
    opts.nocount             = tab_opts.nocount;
    opts.include_actual_plan = include_actual_plan_;
    opts.isolation           = tab_opts.isolation;
    opts.lock_timeout_ms     = tab_opts.lock_timeout_ms;

    const drivers::EngineKind engine = session_info_.engine;
    const drivers::SessionInfo sinfo_copy = session_info_;
    const std::string sql_utf8       = sql.toStdString();
    const uint64_t gen               = ++op_generation_;
    const int64_t started_utc_ms     = core::current_utc_ms();

    // Release previous execution result BEFORE allocating the next one so its Arena OS blocks
    // are immediately returned to the OS (no peak-2x RAM spike and zero dangling refs).
    active_sink_.clear_callback();
    active_sink_.clear_pending();
    result_grid_->clear();
    result_view_states_.clear();
    current_result_.reset();
    active_result_index_ = -1;
    {
        const bool old_blk = result_sets_strip_->blockSignals(true);
        while (result_sets_strip_->count() > 0) {
            result_sets_strip_->removeTab(0);
        }
        result_sets_strip_->setVisible(false);
        result_sets_strip_->blockSignals(old_blk);
    }

    auto result     = std::make_shared<core::QueryExecutionResult>();
    current_result_ = result;
    core::StreamPublishSink* sink_ptr = &active_sink_;

    QPointer<QueryTabWidget> self(this);
    std::weak_ptr<core::QueryExecutionResult> weak_result = result;
    active_sink_.set_callback([self, weak_result, gen]() {
        QMetaObject::invokeMethod(
            self,
            [self, weak_result, gen]() {
                auto locked_res = weak_result.lock();
                if (self && locked_res) {
                    self->on_stream_progress(locked_res, gen);
                } else if (self) {
                    self->active_sink_.clear_pending();
                }
            },
            Qt::QueuedConnection);
    });

    busy_      = true;
    canceling_ = false;
    messages_view_->clear();
    results_text_view_->clear();
    timeout_edit_->setEnabled(false);
    cancel_query_btn_->setVisible(true);
    cancel_query_btn_->setEnabled(true);
    update_action_buttons();
    update_export_availability();
    status_elapsed_->setText(QStringLiteral("00:00:00"));
    update_row_count_segment(0, false);
    set_status_state(StatusKind::Running, QStringLiteral("Executando consulta…"), false);
    exec_started_ = std::chrono::steady_clock::now();
    if (elapsed_timer_ != nullptr) {
        elapsed_timer_->start();
    }
    emit state_changed();

    (void)session_worker_.submit([self,
                                  sql_utf8,
                                  sql,
                                  submitted_start,
                                  submitted_rev,
                                  opts,
                                  tab_opts,
                                  text_fmt,
                                  exec_dest,
                                  rpt_dest_path,
                                  rpt_dest_qstr,
                                  engine,
                                  sinfo_copy,
                                  started_utc_ms,
                                  result,
                                  sink_ptr,
                                  gen](
                                      core::DbSessionWorker& worker,
                                      core::CancelToken& cancel) {
        drivers::DbDriver* drv = worker.driver();
        const auto start_tp = std::chrono::steady_clock::now();
        std::expected<void, drivers::DbError> res = (drv != nullptr && drv->is_connected())
            ? drv->execute_stream(sql_utf8, opts, *result, *sink_ptr, cancel)
            : std::unexpected(drivers::DbError{.message = "Not connected."});
        const double elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();
        result->elapsed_seconds = elapsed_sec;
        result->finished.store(true, std::memory_order_release);
        sink_ptr->clear_callback();

        // Stream Results-to-File (.rpt, UTF-8 BOM + CRLF) directly on the worker thread.
        std::optional<std::expected<core::ExportSummary, std::string>> rpt_res;
        if (exec_dest == core::ResultsDestination::File && !rpt_dest_path.empty()) {
            std::optional<std::string> err_opt;
            if (!res.has_value()) {
                const bool is_cancel = res.error().is_canceled || result->canceled.load(std::memory_order_acquire);
                err_opt = is_cancel ? std::string{"Query canceled by user."} : res.error().format_detailed(engine);
            }
            core::CancelToken file_cancel;
            rpt_res = core::write_results_to_rpt_file(
                *result,
                rpt_dest_path,
                text_fmt,
                elapsed_sec,
                err_opt,
                sql_utf8,
                file_cancel);
        }

        // Parse any captured actual execution plans off the UI thread.
        std::vector<core::ExecutionPlanStatement> parsed_actual_plans;
        if (opts.include_actual_plan) {
            const auto raw_plans = result->actual_plans_snapshot();
            if (!raw_plans.empty()) {
                auto parsed = core::parse_execution_plans(raw_plans, sql_utf8, tab_opts.plan_warnings());
                if (parsed.has_value()) {
                    parsed_actual_plans = std::move(*parsed);
                }
            }
        }

        QMetaObject::invokeMethod(
            self,
            [self,
             res = std::move(res),
             rpt_res = std::move(rpt_res),
             parsed_actual_plans = std::move(parsed_actual_plans),
             sql,
             sql_utf8,
             submitted_start,
             submitted_rev,
             opts,
             tab_opts,
             text_fmt,
             exec_dest,
             rpt_dest_qstr,
             engine,
             sinfo_copy,
             started_utc_ms,
             result,
             gen,
             elapsed_sec]() mutable {
                if (!self || gen != self->op_generation_) {
                    return;
                }
                if (self->elapsed_timer_ != nullptr) {
                    self->elapsed_timer_->stop();
                }
                self->status_elapsed_->setText(format_hhmmss(static_cast<int64_t>(elapsed_sec)));
                self->cancel_query_btn_->setVisible(false);
                self->cancel_query_btn_->setEnabled(false);
                self->timeout_edit_->setEnabled(true);
                self->busy_      = false;
                self->canceling_ = false;
                self->active_sink_.clear_callback();
                self->active_sink_.clear_pending();
                self->update_action_buttons();

                const uint64_t rows     = result->total_rows_read.load(std::memory_order_acquire);
                const int64_t  affected = result->total_rows_affected.load(std::memory_order_acquire);
                const uint32_t rs_count = result->result_set_count.load(std::memory_order_acquire);
                const uint8_t  limits   = result->combined_limit_reasons();
                const bool     trunc    = (limits != core::kLimitNone);

                self->sync_result_sets_ui(*result);

                // Populate Actual Execution Plan tab if requested
                QString plan_unavail_note;
                if (opts.include_actual_plan && self->plan_widget_ != nullptr) {
                    if (!parsed_actual_plans.empty()) {
                        self->plan_widget_->set_statements(std::move(parsed_actual_plans));
                    } else {
                        const std::string unavail = result->actual_plan_unavailable_reason();
                        if (!unavail.empty()) {
                            plan_unavail_note = QString::fromStdString(unavail);
                            self->plan_widget_->set_placeholder_message(plan_unavail_note);
                        } else {
                            self->plan_widget_->set_placeholder_message(
                                QStringLiteral("No actual execution plan was returned by the server for this statement."));
                        }
                    }
                }

                // Build QueryHistoryCapture
                core::QueryHistoryCapture cap;
                cap.sql            = sql_utf8;
                cap.engine         = engine_to_history_name(engine);
                cap.server         = sinfo_copy.host;
                cap.port           = static_cast<int32_t>(sinfo_copy.port);
                cap.database       = sinfo_copy.database;
                cap.user           = sinfo_copy.username;
                cap.started_utc_ms = started_utc_ms;
                cap.duration_ms    = static_cast<int64_t>(elapsed_sec * 1000.0);
                cap.origin         = core::QueryHistoryOrigin::Typed;
                if (rows > 0 || rs_count > 0) {
                    cap.rows_returned = static_cast<int64_t>(rows);
                }
                if (affected >= 0) {
                    cap.rows_affected = affected;
                }

                if (res.has_value()) {
                    cap.outcome = core::QueryHistoryOutcome::Succeeded;
                    QString msg;
                    if (exec_dest == core::ResultsDestination::File) {
                        if (rpt_res.has_value() && rpt_res->has_value()) {
                            msg = QStringLiteral("Query completed. Results written to %1 (%2 row(s), %3).")
                                      .arg(rpt_dest_qstr)
                                      .arg((*rpt_res)->rows_written)
                                      .arg(format_byte_size((*rpt_res)->bytes_written));
                        } else if (rpt_res.has_value()) {
                            msg = QStringLiteral("Query completed, but saving results file failed: %1")
                                      .arg(QString::fromStdString(rpt_res->error()));
                        } else {
                            msg = QStringLiteral("Query completed. %1 row(s).").arg(rows);
                        }
                    } else if (opts.discard_results) {
                        msg = QStringLiteral("Query completed. Results discarded: %1 row(s) read.").arg(rows);
                    } else {
                        msg = QStringLiteral("Query completed. %1 displayed row(s).").arg(rows);
                    }
                    if (affected >= 0 && !opts.nocount) {
                        msg += QStringLiteral("\nRows affected: %1.").arg(affected);
                    }
                    msg += QStringLiteral("\nElapsed: %1 s.").arg(QString::number(elapsed_sec, 'f', 3));
                    if (trunc) {
                        msg += format_limit_warnings(limits);
                    }
                    if (!plan_unavail_note.isEmpty()) {
                        msg += QLatin1Char('\n') + plan_unavail_note;
                    }
                    msg += format_messages_and_completion(*result, tab_opts.show_completion_time);
                    self->messages_view_->setPlainText(msg);

                    if (exec_dest == capybaradb::core::ResultsDestination::Text) {
                        self->results_text_view_->setPlainText(QString::fromStdString(
                            core::format_results_as_text(
                                *result, elapsed_sec, text_fmt, std::nullopt, sql_utf8)));
                        self->output_tab_bar_->setCurrentIndex(0);
                    } else if (exec_dest == capybaradb::core::ResultsDestination::File) {
                        self->output_tab_bar_->setCurrentIndex(1);
                    } else if (rs_count == 0) {
                        self->output_tab_bar_->setCurrentIndex(1);
                    } else {
                        self->output_tab_bar_->setCurrentIndex(0);
                    }

                    self->update_row_count_segment(rows, trunc);
                    if (exec_dest == core::ResultsDestination::File && rpt_res.has_value() && !rpt_res->has_value()) {
                        const QString err_q = QString::fromStdString(rpt_res->error());
                        self->set_status_state(StatusKind::Error, err_q, true);
                        emit self->status_hint(err_q);
                    } else {
                        self->set_status_state(StatusKind::Success, QStringLiteral("Consulta executada com êxito."), false);
                        emit self->status_hint(QStringLiteral("Query completed (%1 row(s), %2 s).")
                                                   .arg(rows)
                                                   .arg(QString::number(elapsed_sec, 'f', 3)));
                    }
                } else {
                    const drivers::DbError& err = res.error();
                    const bool is_cancel = err.is_canceled || result->canceled.load(std::memory_order_acquire);
                    cap.outcome = is_cancel ? core::QueryHistoryOutcome::Canceled : core::QueryHistoryOutcome::Failed;
                    if (!err.sql_state.empty()) {
                        cap.sql_state = err.sql_state;
                    }
                    if (!is_cancel) {
                        cap.error = err.format_short();
                    }

                    QString msg;
                    if (is_cancel) {
                        msg = QStringLiteral("Query canceled. An explicit transaction may require ROLLBACK before further commands.");
                    } else {
                        msg = QString::fromStdString(err.format_detailed(engine));
                        if (err.position > 0 && self->editor_->edit_revision() == submitted_rev) {
                            if (engine == drivers::EngineKind::PostgreSql) {
                                if (!sql.contains(u';')) {
                                    self->set_error_location(submitted_start + (err.position - 1), 0, submitted_rev);
                                }
                            } else {
                                const int base_line = self->editor_->document()->findBlock(submitted_start).blockNumber() + 1;
                                const int target_line = base_line + (err.position - 1);
                                const QTextBlock blk = self->editor_->document()->findBlockByNumber(target_line - 1);
                                if (blk.isValid()) {
                                    self->set_error_location(blk.position(), 0, submitted_rev);
                                }
                            }
                        }
                    }
                    msg += QStringLiteral("\nPartial results: %1 displayed row(s) received before execution stopped. "
                                          "These results do not confirm successful completion or commit.\n"
                                          "An explicit transaction may require ROLLBACK before further commands.")
                               .arg(rows);
                    if (exec_dest == core::ResultsDestination::File && rpt_res.has_value()) {
                        if (rpt_res->has_value()) {
                            msg += QStringLiteral("\nPartial results file written to %1.").arg(rpt_dest_qstr);
                        } else {
                            msg += QStringLiteral("\nSaving results file failed: %1")
                                       .arg(QString::fromStdString(rpt_res->error()));
                        }
                    }
                    msg += format_messages_and_completion(*result, tab_opts.show_completion_time);
                    self->messages_view_->setPlainText(msg);

                    if (exec_dest == capybaradb::core::ResultsDestination::Text) {
                        self->results_text_view_->setPlainText(QString::fromStdString(
                            core::format_results_as_text(
                                *result,
                                elapsed_sec,
                                text_fmt,
                                is_cancel ? std::string{"Query canceled by user."} : err.format_detailed(engine),
                                sql_utf8)));
                    } else {
                        self->output_tab_bar_->setCurrentIndex(1);
                    }

                    self->update_row_count_segment(rows, rows > 0 || trunc);
                    if (is_cancel) {
                        self->set_status_state(StatusKind::Canceled, QStringLiteral("Consulta cancelada pelo usuário."), false);
                        emit self->status_hint(QStringLiteral("Query canceled."));
                    } else {
                        const QString summary = QString::fromStdString(err.format_short());
                        self->set_status_state(StatusKind::Error, summary, true);
                        emit self->status_hint(summary);
                    }
                }
                emit self->query_executed(cap);
                emit self->state_changed();
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::parse_query() {
    if (busy_ || !connected_) {
        return;
    }

    int submitted_start = 0;
    const QString sql = current_submitted_sql(submitted_start);
    const uint64_t submitted_rev = editor_->edit_revision();
    if (sql.trimmed().isEmpty()) {
        return;
    }

    uint32_t unused_timeout = 0;
    if (!validate_before_send(sql, submitted_start, submitted_rev, true, unused_timeout)) {
        return;
    }

    error_offset_.reset();
    go_to_error_btn_->setEnabled(false);

    const drivers::EngineKind engine = session_info_.engine;
    const std::string sql_utf8       = sql.toStdString();
    const uint64_t gen               = ++op_generation_;

    busy_      = true;
    canceling_ = false;
    messages_view_->clear();
    cancel_query_btn_->setVisible(true);
    cancel_query_btn_->setEnabled(true);
    update_action_buttons();
    update_export_availability();
    set_status_state(StatusKind::Running, QStringLiteral("Parsing…"), false);
    exec_started_ = std::chrono::steady_clock::now();
    if (elapsed_timer_ != nullptr) {
        elapsed_timer_->start();
    }
    emit state_changed();

    QPointer<QueryTabWidget> self(this);
    (void)session_worker_.submit([self, sql_utf8, sql, submitted_start, submitted_rev, engine, gen](
                                      core::DbSessionWorker& worker,
                                      core::CancelToken& cancel) {
        drivers::DbDriver* drv = worker.driver();
        const auto start_tp = std::chrono::steady_clock::now();
        std::expected<void, drivers::DbError> res = (drv != nullptr && drv->is_connected())
            ? drv->parse_only(sql_utf8, cancel)
            : std::unexpected(drivers::DbError{.message = "Not connected."});
        const double elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();

        QMetaObject::invokeMethod(
            self,
            [self, res = std::move(res), sql, submitted_start, submitted_rev, engine, gen, elapsed_sec]() {
                if (!self || gen != self->op_generation_) {
                    return;
                }
                if (self->elapsed_timer_ != nullptr) {
                    self->elapsed_timer_->stop();
                }
                self->status_elapsed_->setText(format_hhmmss(static_cast<int64_t>(elapsed_sec)));
                self->cancel_query_btn_->setVisible(false);
                self->cancel_query_btn_->setEnabled(false);
                self->busy_      = false;
                self->canceling_ = false;
                self->update_action_buttons();
                self->update_export_availability();

                if (res.has_value()) {
                    self->messages_view_->setPlainText(QStringLiteral("Commands completed successfully."));
                    self->set_status_state(StatusKind::Success, QStringLiteral("Parse succeeded."), false);
                    emit self->status_hint(QStringLiteral("Parse succeeded."));
                } else {
                    const drivers::DbError& err = res.error();
                    if (err.is_canceled) {
                        self->messages_view_->setPlainText(QStringLiteral("Parse canceled."));
                        self->set_status_state(StatusKind::Canceled, QStringLiteral("Parse canceled."), false);
                    } else {
                        self->messages_view_->setPlainText(QString::fromStdString(err.format_detailed(engine)));
                        if (err.position > 0 && self->editor_->edit_revision() == submitted_rev) {
                            if (engine == drivers::EngineKind::PostgreSql) {
                                self->set_error_location(submitted_start + (err.position - 1), 0, submitted_rev);
                            } else {
                                const int base_line = self->editor_->document()->findBlock(submitted_start).blockNumber() + 1;
                                const int target_line = base_line + (err.position - 1);
                                const QTextBlock blk = self->editor_->document()->findBlockByNumber(target_line - 1);
                                if (blk.isValid()) {
                                    self->set_error_location(blk.position(), 0, submitted_rev);
                                }
                            }
                        }
                        self->set_status_state(StatusKind::Error, QString::fromStdString(err.format_short()), true);
                    }
                }
                self->output_tab_bar_->setCurrentIndex(1);
                emit self->state_changed();
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::run_estimated_plan() {
    run_plan_internal(false);
}

void QueryTabWidget::run_analyze_plan(bool skip_confirm) {
    if (busy_ || !connected_) {
        return;
    }
    if (!skip_confirm) {
        const QString prompt = QStringLiteral(
            "Analyze Plan executes the SQL on the server (including any INSERT, UPDATE, DELETE, or DDL) "
            "to capture actual row counts and timings.\n\nContinue?");
        const bool ok = analyze_confirm_handler_
            ? analyze_confirm_handler_(prompt)
            : (QMessageBox::warning(
                   this,
                   QStringLiteral("Analyze Execution Plan"),
                   prompt,
                   QMessageBox::Yes | QMessageBox::No,
                   QMessageBox::No) == QMessageBox::Yes);
        if (!ok) {
            return;
        }
    }
    run_plan_internal(true);
}

void QueryTabWidget::run_plan_internal(bool analyze) {
    if (busy_ || !connected_) {
        return;
    }
    int submitted_start = 0;
    const QString sql = current_submitted_sql(submitted_start);
    const uint64_t submitted_rev = editor_->edit_revision();
    if (sql.trimmed().isEmpty()) {
        return;
    }

    uint32_t timeout_sec = 0;
    if (!validate_before_send(sql, submitted_start, submitted_rev, false, timeout_sec)) {
        return;
    }

    error_offset_.reset();
    go_to_error_btn_->setEnabled(false);

    const drivers::EngineKind engine = session_info_.engine;
    const std::string sql_utf8       = sql.toStdString();
    const core::ExecutionPlanWarningOptions warn_opts = query_options_.plan_warnings();
    const uint64_t gen = ++op_generation_;

    busy_      = true;
    canceling_ = false;
    cancel_query_btn_->setVisible(true);
    cancel_query_btn_->setEnabled(true);
    update_action_buttons();
    update_export_availability();
    set_status_state(
        StatusKind::Running,
        analyze ? QStringLiteral("Analyzing execution plan…") : QStringLiteral("Requesting estimated execution plan…"),
        false);
    exec_started_ = std::chrono::steady_clock::now();
    if (elapsed_timer_ != nullptr) {
        elapsed_timer_->start();
    }
    emit state_changed();

    QPointer<QueryTabWidget> self(this);
    (void)session_worker_.submit([self, sql_utf8, sql, submitted_start, submitted_rev, analyze, timeout_sec, engine, warn_opts, gen](
                                      core::DbSessionWorker& worker,
                                      core::CancelToken& cancel) {
        drivers::DbDriver* drv = worker.driver();
        const auto start_tp = std::chrono::steady_clock::now();
        auto raw_res = (drv != nullptr && drv->is_connected())
            ? drv->explain_plan(sql_utf8, analyze, timeout_sec, cancel)
            : std::expected<std::vector<core::RawExecutionPlan>, drivers::DbError>(
                  std::unexpected(drivers::DbError{.message = "Not connected."}));
        const double elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();

        std::expected<std::vector<core::ExecutionPlanStatement>, std::string> parsed;
        if (raw_res.has_value()) {
            parsed = core::parse_execution_plans(*raw_res, sql_utf8, warn_opts);
        }

        QMetaObject::invokeMethod(
            self,
            [self,
             raw_res = std::move(raw_res),
             parsed = std::move(parsed),
             sql,
             submitted_start,
             submitted_rev,
             analyze,
             engine,
             gen,
             elapsed_sec]() mutable {
                if (!self || gen != self->op_generation_) {
                    return;
                }
                if (self->elapsed_timer_ != nullptr) {
                    self->elapsed_timer_->stop();
                }
                self->status_elapsed_->setText(format_hhmmss(static_cast<int64_t>(elapsed_sec)));
                self->cancel_query_btn_->setVisible(false);
                self->cancel_query_btn_->setEnabled(false);
                self->busy_      = false;
                self->canceling_ = false;
                self->update_action_buttons();
                self->update_export_availability();

                if (!raw_res.has_value()) {
                    const drivers::DbError& err = raw_res.error();
                    if (err.is_canceled) {
                        self->messages_view_->setPlainText(QStringLiteral("Execution plan request canceled."));
                        self->set_status_state(StatusKind::Canceled, QStringLiteral("Execution plan canceled."), false);
                    } else {
                        self->messages_view_->setPlainText(QString::fromStdString(err.format_detailed(engine)));
                        if (err.position > 0 && self->editor_->edit_revision() == submitted_rev &&
                            engine == drivers::EngineKind::PostgreSql) {
                            self->set_error_location(submitted_start + (err.position - 1), 0, submitted_rev);
                        }
                        self->set_status_state(StatusKind::Error, QString::fromStdString(err.format_short()), true);
                    }
                    self->output_tab_bar_->setCurrentIndex(1);
                    emit self->state_changed();
                    return;
                }

                if (!parsed.has_value()) {
                    const QString parse_err = QString::fromStdString(parsed.error());
                    self->plan_widget_->set_placeholder_message(parse_err);
                    self->messages_view_->setPlainText(parse_err);
                    self->set_status_state(StatusKind::Error, parse_err, true);
                    self->output_tab_bar_->setCurrentIndex(1);
                    emit self->state_changed();
                    return;
                }

                const int stmt_count = static_cast<int>(parsed->size());
                self->plan_widget_->set_statements(std::move(*parsed));
                self->output_tab_bar_->setCurrentIndex(2); // Execution plan tab
                const QString ok_msg = analyze
                    ? QStringLiteral("Analyzed execution plan ready (%1 statement(s), %2 s).")
                          .arg(stmt_count)
                          .arg(QString::number(elapsed_sec, 'f', 3))
                    : QStringLiteral("Estimated execution plan ready (%1 statement(s), %2 s).")
                          .arg(stmt_count)
                          .arg(QString::number(elapsed_sec, 'f', 3));
                self->set_status_state(StatusKind::Success, ok_msg, false);
                emit self->status_hint(ok_msg);
                emit self->state_changed();
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::start_export(core::ExportOptions opts) {
    if (busy_ || exporting_ || !current_result_ || active_result_index_ < 0) {
        return;
    }
    const uint32_t rs_count = current_result_->result_set_count.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(active_result_index_) >= rs_count) {
        return;
    }
    const core::ResultSetStorage* rs_ptr =
        current_result_->result_sets[static_cast<size_t>(active_result_index_)].load(std::memory_order_acquire);
    if (rs_ptr == nullptr || rs_ptr->col_count == 0) {
        return;
    }

    if (static_cast<size_t>(active_result_index_) < result_view_states_.size() &&
        result_view_states_[static_cast<size_t>(active_result_index_)] != nullptr) {
        const auto& vs = *result_view_states_[static_cast<size_t>(active_result_index_)];
        opts.row_order     = vs.row_order;
        opts.display_order = vs.columns.visible_ordinals();
        if (opts.scope == core::ExportScope::SelectedRows) {
            opts.selected_ranges = vs.selection.ranges();
            if (opts.selected_ranges.empty()) {
                const QString msg = QStringLiteral("Select at least one cell or row before exporting selected rows.");
                set_status_state(StatusKind::Idle, msg, false);
                emit status_hint(msg);
                return;
            }
        }
    }
    opts.retain_line_breaks = query_options_.retain_line_breaks;

    QString dest_qstr;
    if (export_file_picker_) {
        dest_qstr = export_file_picker_(opts.format);
    } else {
        const bool is_json = (opts.format == core::ExportFormat::Json);
        dest_qstr = QFileDialog::getSaveFileName(
            this,
            is_json ? QStringLiteral("Export Results to JSON") : QStringLiteral("Export Results to CSV"),
            is_json ? QStringLiteral("results.json") : QStringLiteral("results.csv"),
            is_json ? QStringLiteral("JSON Files (*.json);;All Files (*.*)")
                    : QStringLiteral("CSV Files (*.csv);;All Files (*.*)"));
    }
    if (dest_qstr.trimmed().isEmpty()) {
        return;
    }

    const std::filesystem::path dest_path(dest_qstr.toStdWString());
    exporting_ = true;
    export_cancel_ = std::make_shared<core::CancelToken>();
    const uint64_t exp_gen = ++export_generation_;
    update_export_availability();
    set_status_state(StatusKind::Running, QStringLiteral("Exporting results…"), false);
    emit state_changed();

    // Hold shared_ptr to current_result_ ONLY for the duration of this streaming export job
    // so Arena memory remains valid without copying any cells into RAM.
    std::shared_ptr<core::QueryExecutionResult> pinned_res = current_result_;
    std::shared_ptr<core::CancelToken> cancel_tok = export_cancel_;
    QPointer<QueryTabWidget> self(this);

    effective_worker_pool().submit([self, pinned_res, rs_ptr, dest_path, dest_qstr, opts, cancel_tok, exp_gen]() {
        struct ExportProgressCtx {
            QPointer<QueryTabWidget> self;
            uint64_t                 exp_gen       = 0;
            uint64_t                 last_reported = 0;
        } progress_ctx{self, exp_gen, 0};

        auto progress_fn = +[](uint64_t written, uint64_t total, void* ud) {
            auto* ctx = static_cast<ExportProgressCtx*>(ud);
            if (written >= ctx->last_reported + 25000u || written == total) {
                ctx->last_reported = written;
                QPointer<QueryTabWidget> tab = ctx->self;
                const uint64_t gen = ctx->exp_gen;
                QMetaObject::invokeMethod(
                    tab,
                    [tab, gen, written, total]() {
                        if (tab && tab->exporting_ && gen == tab->export_generation_) {
                            tab->set_status_state(
                                StatusKind::Running,
                                QStringLiteral("Exporting… %1 / %2 row(s)").arg(written).arg(total),
                                false);
                        }
                    },
                    Qt::QueuedConnection);
            }
        };

        auto res = core::export_result_set(
            *rs_ptr,
            dest_path,
            opts,
            *cancel_tok,
            progress_fn,
            &progress_ctx);

        QMetaObject::invokeMethod(
            self,
            [self, res = std::move(res), dest_qstr, exp_gen]() {
                if (!self || exp_gen != self->export_generation_) {
                    return;
                }
                self->exporting_ = false;
                self->export_cancel_.reset();
                self->update_export_availability();

                if (res.has_value()) {
                    const QString msg = QStringLiteral("Exported %1 row(s) (%2) to %3.")
                        .arg(res->rows_written)
                        .arg(format_byte_size(res->bytes_written))
                        .arg(QFileInfo(dest_qstr).fileName());
                    self->set_status_state(StatusKind::Success, msg, false);
                    emit self->status_hint(msg);
                } else {
                    const QString err_q = QString::fromStdString(res.error());
                    const bool is_cancel = err_q.contains(QStringLiteral("canceled"), Qt::CaseInsensitive);
                    self->set_status_state(
                        is_cancel ? StatusKind::Canceled : StatusKind::Error,
                        err_q,
                        !is_cancel);
                    emit self->status_hint(err_q);
                }
                emit self->state_changed();
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::cancel_export() {
    if (!exporting_ || !export_cancel_) {
        return;
    }
    export_cancel_->cancel();
    cancel_export_btn_->setEnabled(false);
    set_status_state(StatusKind::Running, QStringLiteral("Canceling export…"), false);
}

void QueryTabWidget::trigger_intellisense(bool automatic) {
    if (!intellisense_enabled_) {
        if (!automatic) {
            emit status_hint(QStringLiteral("IntelliSense is disabled for this query (Ctrl+B, Ctrl+I to enable)."));
        }
        return;
    }
    if (has_completion_catalog_ && (completion_test_override_ || completion_cache_key_ == current_completion_cache_key())) {
        show_completion_with_catalog(completion_catalog_, automatic);
        return;
    }
    if (!connected_ || !connection_config_.has_value()) {
        // Still allow keyword completions offline when explicitly invoked!
        if (!automatic) {
            core::SqlCompletionCatalog empty_cat;
            show_completion_with_catalog(empty_cat, false);
        }
        return;
    }
    load_completion_catalog_async(true, automatic);
}

void QueryTabWidget::refresh_intellisense_cache() {
    if (!connected_ || !connection_config_.has_value()) {
        emit status_hint(QStringLiteral("Connect to a server to refresh IntelliSense cache."));
        return;
    }
    completion_test_override_ = false;
    completion_catalog_ = {};
    has_completion_catalog_ = false;
    completion_cache_key_.clear();
    if (completion_popup_ != nullptr) {
        completion_popup_->close_popup();
    }
    emit status_hint(QStringLiteral("Refreshing IntelliSense local cache…"));
    load_completion_catalog_async(false, false);
}

void QueryTabWidget::load_completion_catalog_async(bool open_when_ready, bool automatic) {
    if (!connection_config_.has_value()) {
        return;
    }
    if (completion_loading_) {
        completion_pending_open_ = completion_pending_open_ || open_when_ready;
        completion_pending_auto_ = completion_pending_auto_ && automatic;
        return;
    }

    if (completion_load_cancel_) {
        completion_load_cancel_->cancel();
    }
    completion_load_cancel_ = std::make_shared<core::CancelToken>();
    completion_loading_ = true;
    completion_pending_open_ = open_when_ready;
    completion_pending_auto_ = automatic;
    const uint64_t gen = ++completion_load_gen_;

    const drivers::ConnectionConfig cfg = *connection_config_;
    const std::string key = current_completion_cache_key();
    auto token = completion_load_cancel_;
    auto factory = catalog_provider_factory_;
    QPointer<QueryTabWidget> self(this);

    effective_worker_pool().submit([self, cfg, key, token, factory, gen]() {
        auto provider = factory(cfg.engine);
        auto res = provider ? provider->load_completion_catalog(cfg, *token)
                            : std::expected<core::SqlCompletionCatalog, drivers::DbError>(
                                  std::unexpected(drivers::DbError{.message = "No catalog provider."}));

        QMetaObject::invokeMethod(
            self,
            [self, res = std::move(res), key, gen]() mutable {
                if (!self || gen != self->completion_load_gen_) {
                    return;
                }
                self->completion_loading_ = false;
                const bool should_open = self->completion_pending_open_;
                const bool was_auto    = self->completion_pending_auto_;
                self->completion_pending_open_ = false;
                self->completion_pending_auto_ = false;

                if (!res.has_value()) {
                    if (!was_auto) {
                        emit self->status_hint(
                            QStringLiteral("IntelliSense cache refresh failed: %1")
                                .arg(QString::fromStdString(res.error().format_short())));
                    }
                    return;
                }

                self->completion_catalog_     = std::move(*res);
                self->has_completion_catalog_ = true;
                self->completion_cache_key_   = key;
                if (should_open) {
                    self->show_completion_with_catalog(self->completion_catalog_, was_auto);
                } else {
                    emit self->status_hint(QStringLiteral("IntelliSense local cache refreshed."));
                }
            },
            Qt::QueuedConnection);
    });
}

void QueryTabWidget::show_completion_with_catalog(const core::SqlCompletionCatalog& catalog, bool automatic) {
    if (completion_popup_ == nullptr || editor_ == nullptr) {
        return;
    }
    const QString text = editor_->toPlainText();
    const int caret_char = editor_->textCursor().position();
    const size_t caret_utf8 = SqlCodeEditor::char_pos_to_utf8_offset(text, caret_char);
    const std::string utf8 = text.toStdString();
    const auto result = core::complete_sql(utf8, caret_utf8, catalog);
    if (!result.has_value() || result->items.empty()) {
        completion_popup_->close_popup();
        return;
    }
    if (automatic && result->items.empty()) {
        completion_popup_->close_popup();
        return;
    }

    const int replace_start_char = SqlCodeEditor::utf8_offset_to_char_pos(text, result->replace_start);
    const QString prefix = QString::fromStdString(result->prefix);
    const QPoint global_pos = editor_->completion_popup_global_pos();
    completion_popup_->open_at(global_pos, replace_start_char, prefix, result->items);
}

void QueryTabWidget::update_open_completion_filter() {
    if (completion_popup_ == nullptr || !completion_popup_->is_open()) {
        return;
    }
    const int caret = editor_->textCursor().position();
    const int start = completion_popup_->replace_start_char_pos();
    if (caret < start) {
        completion_popup_->close_popup();
        return;
    }
    const QString text = editor_->toPlainText();
    const QString prefix = text.mid(start, caret - start);
    for (const QChar ch : prefix) {
        if (!is_completion_filter_char(ch)) {
            completion_popup_->close_popup();
            return;
        }
    }
    completion_popup_->update_filter(prefix);
    if (completion_popup_->visible_count() == 0) {
        completion_popup_->close_popup();
    }
}

void QueryTabWidget::accept_completion_item(const QString& text, int replace_start_char_pos) {
    if (completion_popup_ != nullptr) {
        completion_popup_->close_popup();
    }
    const int caret = editor_->textCursor().position();
    editor_->replace_text_range(replace_start_char_pos, std::max(replace_start_char_pos, caret), text);
    editor_->setFocus(Qt::ShortcutFocusReason);
}

void QueryTabWidget::cancel_query() {
    if (!busy_ || canceling_) {
        return;
    }
    canceling_ = true;
    cancel_query_btn_->setEnabled(false);
    set_status_state(StatusKind::Running, QStringLiteral("Cancelando consulta…"), false);
    emit state_changed();
    session_worker_.cancel_active();
}

std::expected<drivers::SessionLossRisk, drivers::DbError> QueryTabWidget::inspect_loss_risk_sync() {
    if (!connected_ || busy_) {
        return drivers::SessionLossRisk{};
    }
    struct SyncState {
        std::expected<drivers::SessionLossRisk, drivers::DbError> result;
        QPointer<QEventLoop>                                      loop;
        std::atomic<bool>                                         finished{false};
    } state;

    QEventLoop loop;
    state.loop = &loop;

    QTimer guard_timer;
    guard_timer.setSingleShot(true);
    connect(&guard_timer, &QTimer::timeout, &loop, [this, &loop]() {
        session_worker_.cancel_active();
        loop.quit();
    });
    guard_timer.start(3000);

    (void)session_worker_.submit([&state](core::DbSessionWorker& worker, core::CancelToken& cancel) {
        if (auto* drv = worker.driver(); drv != nullptr && drv->is_connected()) {
            state.result = drv->inspect_loss_risk(cancel);
        }
        if (state.loop) {
            QMetaObject::invokeMethod(state.loop, &QEventLoop::quit, Qt::QueuedConnection);
        }
        state.finished.store(true, std::memory_order_release);
        state.finished.notify_all();
    });
    loop.exec();
    while (!state.finished.load(std::memory_order_acquire)) {
        state.finished.wait(false, std::memory_order_acquire);
    }
    return state.result;
}

std::expected<void, drivers::DbError> QueryTabWidget::commit_transaction_sync() {
    if (!connected_ || busy_) {
        return {};
    }
    struct SyncState {
        std::expected<void, drivers::DbError> result;
        QPointer<QEventLoop>                  loop;
        std::atomic<bool>                     finished{false};
    } state;

    QEventLoop loop;
    state.loop = &loop;

    QTimer guard_timer;
    guard_timer.setSingleShot(true);
    connect(&guard_timer, &QTimer::timeout, &loop, [this, &loop]() {
        session_worker_.cancel_active();
        loop.quit();
    });
    guard_timer.start(5000);

    (void)session_worker_.submit([&state](core::DbSessionWorker& worker, core::CancelToken& cancel) {
        if (auto* drv = worker.driver(); drv != nullptr && drv->is_connected()) {
            state.result = drv->commit_transaction(cancel);
        }
        if (state.loop) {
            QMetaObject::invokeMethod(state.loop, &QEventLoop::quit, Qt::QueuedConnection);
        }
        state.finished.store(true, std::memory_order_release);
        state.finished.notify_all();
    });
    loop.exec();
    while (!state.finished.load(std::memory_order_acquire)) {
        state.finished.wait(false, std::memory_order_acquire);
    }
    return state.result;
}

} // namespace capybaradb::ui
