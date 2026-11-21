#include "ui/shell/main_window.hpp"

#include "core/config/connection_profile.hpp"
#include "core/config/query_recovery.hpp"
#include "core/config/state_paths.hpp"
#include "drivers/common/driver_registry.hpp"
#include "ui/dialogs/connection_dialog.hpp"
#include "ui/dialogs/options_dialog.hpp"
#include "ui/dialogs/query_history_dialog.hpp"
#include "ui/dialogs/query_options_dialog.hpp"
#include "ui/editor/query_tab_widget.hpp"
#include "ui/editor/sql_code_editor.hpp"
#include "ui/explorer/object_explorer_panel.hpp"
#include "ui/explorer/registered_servers_panel.hpp"
#include "ui/grid/result_grid_viewport.hpp"
#include "ui/plan/execution_plan_widget.hpp"
#include "ui/shell/icon_catalog.hpp"
#include "ui/shell/shortcut_router.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <algorithm>
#include <functional>
#include <vector>

namespace capybaradb::ui {
namespace {

capybaradb::core::ScreenRect to_core_rect(const QRect& r) noexcept {
    return capybaradb::core::ScreenRect{r.x(), r.y(), r.width(), r.height()};
}

class AvailableDatabasesComboBox final : public QComboBox {
public:
    explicit AvailableDatabasesComboBox(QWidget* parent = nullptr)
        : QComboBox(parent) {}

    std::function<void()> on_popup;

    void showPopup() override {
        if (on_popup) {
            on_popup();
        }
        QComboBox::showPopup();
    }
};

} // namespace

MainWindow::MainWindow(std::filesystem::path state_dir, QWidget* parent)
    : QMainWindow(parent)
    , state_dir_(state_dir.empty() ? capybaradb::core::state_directory() : std::move(state_dir)) {
    setObjectName(QStringLiteral("CapybaraMainWindow"));
    setWindowTitle(QStringLiteral("CapybaraDB"));

    create_actions();
    create_menu_bar();
    create_tool_bars();
    create_status_bar();
    create_dock_manager();
    connect_shortcuts();

    apply_default_window_geometry();
    restore_persistent_state();
    refresh_commands();

    auto* plugin_gc_timer = new QTimer(this);
    plugin_gc_timer->setInterval(5000);
    connect(plugin_gc_timer, &QTimer::timeout, this, []() {
        (void)capybaradb::drivers::unload_unused_driver_modules(std::chrono::seconds{5});
    });
    plugin_gc_timer->start();
}

MainWindow::~MainWindow() {
    if (shortcut_router_ != nullptr) {
        qApp->removeEventFilter(shortcut_router_);
    }
    if (db_list_cancel_) {
        db_list_cancel_->cancel();
    }
    if (explorer_panel_ != nullptr && explorer_panel_->catalog_model() != nullptr) {
        explorer_panel_->catalog_model()->cancel_all_loads();
    }
    for (const QueryEntry& e : query_entries_) {
        if (e.widget != nullptr) {
            e.widget->disconnect_session();
        }
    }
    worker_pool_.stop();
    (void)capybaradb::drivers::unload_unused_driver_modules();
}

void MainWindow::create_actions() {
    // File
    act_new_query_ = new QAction(get_icon(IconId::NewQuery), QStringLiteral("New &Query\tCtrl+N"), this);
    act_new_query_->setToolTip(QStringLiteral("New Query (Ctrl+N)"));
    connect(act_new_query_, &QAction::triggered, this, [this]() { new_query_in_context(); });

    act_open_ = new QAction(get_icon(IconId::Open), QStringLiteral("&Open…"), this);
    act_open_->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    act_open_->setToolTip(QStringLiteral("Open File (Ctrl+O)"));
    connect(act_open_, &QAction::triggered, this, &MainWindow::open_files_dialog);

    act_open_plan_ = new QAction(QStringLiteral("Open Execution &Plan…"), this);
    connect(act_open_plan_, &QAction::triggered, this, &MainWindow::open_execution_plan_file);

    act_save_ = new QAction(get_icon(IconId::Save), QStringLiteral("&Save"), this);
    act_save_->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    act_save_->setToolTip(QStringLiteral("Save (Ctrl+S)"));
    connect(act_save_, &QAction::triggered, this, [this]() {
        if (QueryTabWidget* q = active_query()) {
            (void)save_query_tab(q, false);
        }
    });

    act_save_as_ = new QAction(QStringLiteral("Save &As…"), this);
    connect(act_save_as_, &QAction::triggered, this, [this]() {
        if (QueryTabWidget* q = active_query()) {
            (void)save_query_tab(q, true);
        }
    });

    act_close_tab_ = new QAction(QStringLiteral("&Close Tab"), this);
    connect(act_close_tab_, &QAction::triggered, this, &MainWindow::close_active_query);

    act_exit_ = new QAction(QStringLiteral("E&xit"), this);
    connect(act_exit_, &QAction::triggered, this, &QWidget::close);

    // Edit
    act_undo_          = new QAction(QStringLiteral("&Undo"), this);
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->editor_widget()->undo();
    });
    act_redo_          = new QAction(QStringLiteral("&Redo"), this);
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->editor_widget()->redo();
    });
    act_cut_           = new QAction(QStringLiteral("Cu&t"), this);
    connect(act_cut_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->editor_widget()->cut();
    });
    act_copy_          = new QAction(QStringLiteral("&Copy"), this);
    connect(act_copy_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) {
            QWidget* fw = QApplication::focusWidget();
            if (fw != nullptr && q->result_grid() != nullptr &&
                (fw == q->result_grid() || q->result_grid()->isAncestorOf(fw))) {
                (void)q->result_grid()->copy_selection(false);
            } else {
                q->editor_widget()->copy();
            }
        }
    });
    act_paste_         = new QAction(QStringLiteral("&Paste"), this);
    connect(act_paste_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->editor_widget()->paste();
    });
    act_find_          = new QAction(QStringLiteral("&Find…"), this);
    act_find_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    connect(act_find_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->show_find_bar();
    });
    act_go_to_line_    = new QAction(QStringLiteral("Go To &Line…"), this);
    act_go_to_line_->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
    connect(act_go_to_line_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->show_go_to_line_bar();
    });
    act_replace_       = new QAction(QStringLiteral("&Replace…"), this);
    act_replace_->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(act_replace_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->show_replace_bar();
    });
    act_comment_       = new QAction(QStringLiteral("Comment Selection\tCtrl+K, Ctrl+C"), this);
    connect(act_comment_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->comment_selection(false);
    });
    act_uncomment_     = new QAction(QStringLiteral("Uncomment Selection\tCtrl+K, Ctrl+U"), this);
    connect(act_uncomment_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->comment_selection(true);
    });
    act_list_members_  = new QAction(QStringLiteral("List &Members"), this);
    act_list_members_->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    connect(act_list_members_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->trigger_intellisense(false);
    });
    act_complete_word_ = new QAction(QStringLiteral("Complete &Word"), this);
    act_complete_word_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Space")));
    connect(act_complete_word_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->trigger_intellisense(false);
    });
    act_refresh_cache_ = new QAction(QStringLiteral("&Refresh Local Cache"), this);
    act_refresh_cache_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(act_refresh_cache_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->refresh_intellisense_cache();
    });

    // View
    act_show_explorer_ = new QAction(QStringLiteral("Object &Explorer\tF8"), this);
    connect(act_show_explorer_, &QAction::triggered, this, &MainWindow::show_object_explorer);

    act_reg_servers_   = new QAction(QStringLiteral("Re&gistered Servers"), this);
    act_reg_servers_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+G")));
    connect(act_reg_servers_, &QAction::triggered, this, &MainWindow::show_registered_servers);
    act_activity_mon_  = new QAction(QStringLiteral("Activity &Monitor"), this);
    act_activity_mon_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+A")));
    act_query_stats_   = new QAction(QStringLiteral("Query Statistics…"), this);
    act_query_history_ = new QAction(QStringLiteral("Query &History…"), this);
    act_query_history_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+H")));
    connect(act_query_history_, &QAction::triggered, this, &MainWindow::show_query_history_dialog);

    act_word_wrap_ = new QAction(QStringLiteral("Word Wrap"), this);
    act_word_wrap_->setCheckable(true);
    connect(act_word_wrap_, &QAction::toggled, this, [this](bool on) {
        if (auto* q = active_query()) {
            q->set_word_wrap(on);
        }
    });

    act_white_space_ = new QAction(QStringLiteral("Show White Space"), this);
    act_white_space_->setCheckable(true);
    connect(act_white_space_, &QAction::toggled, this, [this](bool on) {
        if (auto* q = active_query()) {
            q->set_show_white_space(on);
        }
    });

    // Query
    act_conn_connect_     = new QAction(QStringLiteral("&Connect…"), this);
    connect(act_conn_connect_, &QAction::triggered, this, &MainWindow::connect_active_query_with_dialog);

    act_conn_disconnect_  = new QAction(QStringLiteral("&Disconnect"), this);
    act_conn_disconnect_->setEnabled(false);
    connect(act_conn_disconnect_, &QAction::triggered, this, &MainWindow::disconnect_active_query);

    act_conn_change_      = new QAction(QStringLiteral("Change C&onnection…"), this);
    act_conn_change_->setEnabled(false);
    connect(act_conn_change_, &QAction::triggered, this, &MainWindow::change_active_query_connection);

    act_open_in_explorer_ = new QAction(QStringLiteral("Open Ser&ver in Object Explorer"), this);
    act_open_in_explorer_->setShortcut(QKeySequence(QStringLiteral("Alt+F8")));
    act_open_in_explorer_->setEnabled(false);
    connect(act_open_in_explorer_, &QAction::triggered, this, &MainWindow::open_active_server_in_explorer);

    act_execute_ = new QAction(get_icon(IconId::ExecuteDisabled), QStringLiteral("&Execute\tAlt+X"), this);
    act_execute_->setIconText(QStringLiteral("Execute"));
    act_execute_->setToolTip(QStringLiteral("Execute (F5)"));
    act_execute_->setEnabled(false);
    connect(act_execute_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) {
            q->execute_query();
        }
    });

    act_cancel_ = new QAction(get_icon(IconId::CancelDisabled), QStringLiteral("&Cancel Executing Query"), this);
    act_cancel_->setToolTip(QStringLiteral("Cancel Executing Query (Alt+Break)"));
    act_cancel_->setEnabled(false);
    connect(act_cancel_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) {
            q->cancel_query();
        }
    });

    act_parse_ = new QAction(QStringLiteral("Pa&rse"), this);
    act_parse_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F5")));
    act_parse_->setEnabled(false);
    connect(act_parse_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) {
            q->parse_query();
        }
    });

    act_est_plan_ = new QAction(get_icon(IconId::PlanEstimated), QStringLiteral("Display E&stimated Execution Plan"), this);
    act_est_plan_->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    act_est_plan_->setToolTip(QStringLiteral("Display Estimated Execution Plan (Ctrl+L)"));
    act_est_plan_->setEnabled(false);
    connect(act_est_plan_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) q->run_estimated_plan();
    });

    act_intellisense_ = new QAction(QStringLiteral("&IntelliSense Enabled\tCtrl+B, Ctrl+I"), this);
    act_intellisense_->setCheckable(true);
    act_intellisense_->setEnabled(false);
    connect(act_intellisense_, &QAction::triggered, this, [this]() {
        if (auto* q = active_query()) {
            q->set_intellisense_enabled(!q->intellisense_enabled());
            refresh_commands();
        }
    });

    act_design_query_ = new QAction(QStringLiteral("&Design Query in Editor…"), this);
    act_design_query_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));

    act_actual_plan_ = new QAction(get_icon(IconId::PlanActual), QStringLiteral("Include Actual Execution &Plan"), this);
    act_actual_plan_->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    act_actual_plan_->setToolTip(QStringLiteral("Include Actual Execution Plan (Ctrl+M)"));
    act_actual_plan_->setCheckable(true);
    act_actual_plan_->setEnabled(false);
    connect(act_actual_plan_, &QAction::triggered, this, [this](bool checked) {
        if (auto* q = active_query()) {
            q->set_include_actual_plan(checked);
            refresh_commands();
        }
    });

    act_client_stats_ = new QAction(QStringLiteral("Include C&lient Statistics"), this);
    act_client_stats_->setShortcut(QKeySequence(QStringLiteral("Shift+Alt+S")));
    act_client_stats_->setCheckable(true);
    act_client_stats_->setEnabled(false);
    connect(act_client_stats_, &QAction::triggered, this, [this](bool checked) {
        if (auto* q = active_query()) {
            q->set_include_client_statistics(checked);
            refresh_commands();
        }
    });

    act_reset_stats_ = new QAction(QStringLiteral("Reset Client Statistics"), this);
    act_reset_stats_->setEnabled(false);

    act_results_group_ = new QActionGroup(this);
    act_results_group_->setExclusive(true);

    act_results_text_ = new QAction(get_icon(IconId::ResultsText), QStringLiteral("Results to &Text"), this);
    act_results_text_->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    act_results_text_->setToolTip(QStringLiteral("Results to Text (Ctrl+T)"));
    act_results_text_->setCheckable(true);
    act_results_text_->setEnabled(false);
    act_results_group_->addAction(act_results_text_);
    connect(act_results_text_, &QAction::triggered, this, [this]() {
        set_results_destination(capybaradb::core::ResultsDestination::Text);
    });

    act_results_grid_ = new QAction(get_icon(IconId::ResultsGrid), QStringLiteral("Results to &Grid"), this);
    act_results_grid_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    act_results_grid_->setToolTip(QStringLiteral("Results to Grid (Ctrl+D)"));
    act_results_grid_->setCheckable(true);
    act_results_grid_->setEnabled(false);
    act_results_group_->addAction(act_results_grid_);
    connect(act_results_grid_, &QAction::triggered, this, [this]() {
        set_results_destination(capybaradb::core::ResultsDestination::Grid);
    });

    act_results_file_ = new QAction(get_icon(IconId::ResultsFile), QStringLiteral("Results to &File"), this);
    act_results_file_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    act_results_file_->setToolTip(QStringLiteral("Results to File (Ctrl+Shift+F)"));
    act_results_file_->setCheckable(true);
    act_results_file_->setEnabled(false);
    act_results_group_->addAction(act_results_file_);
    connect(act_results_file_, &QAction::triggered, this, [this]() {
        set_results_destination(capybaradb::core::ResultsDestination::File);
    });

    act_query_options_ = new QAction(get_icon(IconId::QueryOptions), QStringLiteral("Query &Options…"), this);
    act_query_options_->setToolTip(QStringLiteral("Query Options"));
    act_query_options_->setEnabled(false);
    connect(act_query_options_, &QAction::triggered, this, &MainWindow::open_query_options_dialog);

    act_gen_sql_ai_ = new QAction(QStringLiteral("Generate SQL with &AI…\tCtrl+I"), this);

    // Tools, Window & Help
    act_options_ = new QAction(QStringLiteral("&Options…"), this);
    connect(act_options_, &QAction::triggered, this, &MainWindow::open_environment_options_dialog);

    act_reset_layout_ = new QAction(QStringLiteral("Reset Window &Layout"), this);
    connect(act_reset_layout_, &QAction::triggered, this, &MainWindow::reset_window_layout);

    act_about_ = new QAction(QStringLiteral("&About CapybaraDB"), this);
    connect(act_about_, &QAction::triggered, this, &MainWindow::show_about_dialog);
}

void MainWindow::create_menu_bar() {
    auto* mb = menuBar();
    mb->setObjectName(QStringLiteral("MainMenuBar"));

    auto* file_menu = mb->addMenu(QStringLiteral("&File"));
    file_menu->addAction(act_new_query_);
    file_menu->addAction(act_open_);
    file_menu->addAction(act_open_plan_);
    file_menu->addSeparator();
    file_menu->addAction(act_save_);
    file_menu->addAction(act_save_as_);
    file_menu->addSeparator();
    file_menu->addAction(act_exit_);

    auto* edit_menu = mb->addMenu(QStringLiteral("&Edit"));
    edit_menu->addAction(act_undo_);
    edit_menu->addAction(act_redo_);
    edit_menu->addSeparator();
    edit_menu->addAction(act_cut_);
    edit_menu->addAction(act_copy_);
    edit_menu->addAction(act_paste_);
    edit_menu->addSeparator();
    edit_menu->addAction(act_find_);
    edit_menu->addAction(act_go_to_line_);
    edit_menu->addAction(act_replace_);
    edit_menu->addSeparator();
    edit_menu->addAction(act_comment_);
    auto* intellisense_menu = edit_menu->addMenu(QStringLiteral("&IntelliSense"));
    intellisense_menu->addAction(act_list_members_);
    intellisense_menu->addAction(act_complete_word_);
    intellisense_menu->addAction(act_refresh_cache_);
    edit_menu->addAction(act_uncomment_);

    auto* view_menu = mb->addMenu(QStringLiteral("&View"));
    view_menu->addAction(act_show_explorer_);
    view_menu->addAction(act_reg_servers_);
    view_menu->addAction(act_activity_mon_);
    view_menu->addAction(act_query_stats_);
    view_menu->addAction(act_query_history_);
    view_menu->addSeparator();
    view_menu->addAction(act_word_wrap_);
    view_menu->addAction(act_white_space_);

    auto* query_menu = mb->addMenu(QStringLiteral("&Query"));
    auto* conn_menu = query_menu->addMenu(QStringLiteral("Co&nnection"));
    conn_menu->addAction(act_conn_connect_);
    conn_menu->addAction(act_conn_disconnect_);
    conn_menu->addAction(act_conn_change_);
    query_menu->addAction(act_open_in_explorer_);
    query_menu->addSeparator();
    act_execute_->setText(QStringLiteral("&Execute\tAlt+X"));
    act_execute_->setIconText(QStringLiteral("Execute"));
    query_menu->addAction(act_execute_);
    query_menu->addAction(act_cancel_);
    query_menu->addSeparator();
    query_menu->addAction(act_parse_);
    query_menu->addAction(act_est_plan_);
    query_menu->addAction(act_intellisense_);
    query_menu->addSeparator();
    query_menu->addAction(act_design_query_);
    query_menu->addSeparator();
    query_menu->addAction(act_actual_plan_);
    query_menu->addAction(act_client_stats_);
    query_menu->addAction(act_reset_stats_);
    query_menu->addSeparator();
    auto* results_menu = query_menu->addMenu(QStringLiteral("Results &To"));
    results_menu->addAction(act_results_text_);
    results_menu->addAction(act_results_grid_);
    results_menu->addAction(act_results_file_);
    query_menu->addAction(act_query_options_);
    query_menu->addSeparator();
    query_menu->addAction(act_gen_sql_ai_);

    auto* tools_menu = mb->addMenu(QStringLiteral("&Tools"));
    for (const char* item : {
             "New &Database…", "New &Schema…", "Schema Operations…", "New Table…",
             "Sequence Operations…", "Table Properties…", "Table Operations…",
             "Index Operations…", "Constraint Operations…", "CHECK Constraint Operations…",
             "Foreign Key Operations…", "Database Operations…", "New Login/Group &Role…"}) {
        auto* act = tools_menu->addAction(QString::fromUtf8(item));
        act->setEnabled(false);
    }
    tools_menu->addSeparator();
    for (const char* item : {
             "Back Up Database…", "Restore Database…",
             "Back Up Global Objects…", "Restore Global Objects…"}) {
        auto* act = tools_menu->addAction(QString::fromUtf8(item));
        act->setEnabled(false);
    }
    tools_menu->addSeparator();
    tools_menu->addAction(QStringLiteral("AI Assistant Settings…"));
    tools_menu->addAction(act_options_);

    auto* perms_menu = mb->addMenu(QStringLiteral("&Permissions"));
    for (const char* item : {
             "Object Permissions…", "Default Permissions…", "Role Properties…",
             "Role Memberships…", "Rename Role…", "Drop Role…", "Set Role Password…"}) {
        auto* act = perms_menu->addAction(QString::fromUtf8(item));
        act->setEnabled(false);
    }

    auto* window_menu = mb->addMenu(QStringLiteral("&Window"));
    window_menu->addAction(act_reset_layout_);

    auto* help_menu = mb->addMenu(QStringLiteral("&Help"));
    help_menu->addAction(act_about_);
}

void MainWindow::create_tool_bars() {
    // Band 0
    toolbar_band0_ = addToolBar(QStringLiteral("Standard"));
    toolbar_band0_->setObjectName(QStringLiteral("MainToolBarBand0"));
    toolbar_band0_->setMovable(false);
    toolbar_band0_->setIconSize(QSize(theme::kIconSizePx, theme::kIconSizePx));
    toolbar_band0_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    toolbar_band0_->addAction(act_open_);
    auto* save_btn_action = toolbar_band0_->addAction(get_icon(IconId::Save), QStringLiteral("Save"));
    save_btn_action->setToolTip(QStringLiteral("Save (Ctrl+S)"));
    connect(save_btn_action, &QAction::triggered, act_save_, &QAction::trigger);
    // Show icon-only on Save button
    if (QWidget* w = toolbar_band0_->widgetForAction(save_btn_action)) {
        if (auto* tb = qobject_cast<QToolButton*>(w)) {
            tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
    }
    toolbar_band0_->addSeparator();
    auto* new_q_btn_action = toolbar_band0_->addAction(get_icon(IconId::NewQuery), QStringLiteral("New Query"));
    new_q_btn_action->setToolTip(QStringLiteral("New Query (Ctrl+N)"));
    connect(new_q_btn_action, &QAction::triggered, act_new_query_, &QAction::trigger);

    addToolBarBreak(Qt::TopToolBarArea);

    // Band 1
    toolbar_band1_ = addToolBar(QStringLiteral("SQL Editor"));
    toolbar_band1_->setObjectName(QStringLiteral("MainToolBarBand1"));
    toolbar_band1_->setMovable(false);
    toolbar_band1_->setIconSize(QSize(theme::kIconSizePx, theme::kIconSizePx));
    toolbar_band1_->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto* connect_action = toolbar_band1_->addAction(QStringLiteral("Connect…"));
    connect_action->setToolTip(QStringLiteral("Connect to server"));
    connect(connect_action, &QAction::triggered, this, &MainWindow::connect_with_dialog);
    toolbar_band1_->addSeparator();

    auto* combo = new AvailableDatabasesComboBox(toolbar_band1_);
    combo->on_popup = [this]() { populate_databases_popup(); };
    db_combo_ = combo;
    db_combo_->setObjectName(QStringLiteral("AvailableDatabasesCombo"));
    db_combo_->setFixedWidth(theme::kAvailableDbComboWidthPx);
    db_combo_->setEnabled(false);
    db_combo_->setAccessibleName(QStringLiteral("Available Databases"));
    connect(db_combo_, &QComboBox::activated, this, &MainWindow::on_database_activated);
    toolbar_band1_->addWidget(db_combo_);

    toolbar_band1_->addAction(act_execute_);
    if (QWidget* w = toolbar_band1_->widgetForAction(act_execute_)) {
        if (auto* tb = qobject_cast<QToolButton*>(w)) {
            tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            tb->setText(QStringLiteral("Execute"));
        }
    }
    toolbar_band1_->addAction(act_cancel_);
    toolbar_band1_->addSeparator();
    toolbar_band1_->addAction(act_est_plan_);
    toolbar_band1_->addAction(act_actual_plan_);
    toolbar_band1_->addSeparator();
    toolbar_band1_->addAction(act_results_text_);
    toolbar_band1_->addAction(act_results_grid_);
    toolbar_band1_->addAction(act_results_file_);
    toolbar_band1_->addAction(act_query_options_);
}

void MainWindow::create_status_bar() {
    global_status_bar_ = new QStatusBar(this);
    global_status_bar_->setObjectName(QStringLiteral("GlobalStatusBar"));
    global_status_bar_->setSizeGripEnabled(false);

    status_label_ = new QLabel(QStringLiteral("Ready"), global_status_bar_);
    build_label_  = new QLabel(QStringLiteral("CapybaraDB · Development build"), global_status_bar_);
    global_status_bar_->addWidget(status_label_, 1);
    global_status_bar_->addPermanentWidget(build_label_);
    setStatusBar(global_status_bar_);
}

void MainWindow::create_dock_manager() {
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHideDisabledButtons, true);
    ads::CDockManager::setAutoHideConfigFlag(ads::CDockManager::AutoHideFeatureEnabled, true);

    dock_manager_ = new ads::CDockManager(this);
    dock_manager_->setObjectName(QStringLiteral("DockManager"));
    // Clear ADS's built-in stylesheet so assets/themes/classic_blue.qss styles all dock chrome.
    dock_manager_->setStyleSheet(QString{});
    setCentralWidget(dock_manager_);

    auto* workspace_canvas = new QWidget(this);
    workspace_canvas->setObjectName(QStringLiteral("WorkspaceCanvas"));
    workspace_canvas->setAttribute(Qt::WA_StyledBackground, true);
    workspace_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    workspace_dock_ = new ads::CDockWidget(QStringLiteral("Workspace"));
    workspace_dock_->setObjectName(QStringLiteral("workspace_canvas"));
    workspace_dock_->setWidget(workspace_canvas);
    workspace_dock_->setFeature(ads::CDockWidget::NoTab, true);
    workspace_dock_->setFeature(ads::CDockWidget::DockWidgetFocusable, false);
    dock_manager_->setCentralWidget(workspace_dock_);

    explorer_panel_ = new ObjectExplorerPanel(state_dir_ / "explorer-toolbar.json", &worker_pool_, this);
    connect(explorer_panel_, &ObjectExplorerPanel::status_message, this, [this](const QString& msg) {
        status_label_->setText(msg);
    });
    connect(explorer_panel_, &ObjectExplorerPanel::connect_requested, this, &MainWindow::connect_with_dialog);
    connect(explorer_panel_, &ObjectExplorerPanel::new_query_requested, this, [this](const capybaradb::drivers::ConnectionConfig& cfg) {
        if (QueryTabWidget* q = new_query()) {
            q->connect_async(cfg, &worker_pool_);
        }
    });
    connect(explorer_panel_, &ObjectExplorerPanel::select_top_requested, this, &MainWindow::run_select_top);
    connect(explorer_panel_, &ObjectExplorerPanel::script_select_requested, this, &MainWindow::run_script_select);

    explorer_dock_ = new ads::CDockWidget(QStringLiteral("Object Explorer"));
    explorer_dock_->setObjectName(QStringLiteral("explorer"));
    explorer_dock_->setWidget(explorer_panel_);
    explorer_dock_->setFeature(ads::CDockWidget::DockWidgetClosable, false);
    explorer_dock_->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, false);
    explorer_dock_->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    explorer_dock_->resize(theme::kObjectExplorerInitialWidthPx, 600);
    explorer_dock_->setMinimumSize(180, 150);

    ads::CDockAreaWidget* explorer_area =
        dock_manager_->addDockWidget(ads::LeftDockWidgetArea, explorer_dock_);
    if (explorer_area != nullptr) {
        explorer_area->setObjectName(QStringLiteral("ExplorerDockArea"));
    }

    reg_servers_panel_ = new RegisteredServersPanel(state_dir_, &worker_pool_, this);
    connect(reg_servers_panel_, &RegisteredServersPanel::connect_in_explorer_requested,
            this, &MainWindow::connect_explorer_with_config);
    connect(reg_servers_panel_, &RegisteredServersPanel::new_query_requested,
            this, [this](const capybaradb::drivers::ConnectionConfig& cfg) {
        if (QueryTabWidget* q = new_query()) {
            q->connect_async(cfg, &worker_pool_);
        }
    });
    connect(reg_servers_panel_, &RegisteredServersPanel::status_message, this, [this](const QString& msg) {
        status_label_->setText(msg);
    });

    reg_servers_dock_ = new ads::CDockWidget(QStringLiteral("Registered Servers"));
    reg_servers_dock_->setObjectName(QStringLiteral("registered_servers"));
    reg_servers_dock_->setWidget(reg_servers_panel_);
    reg_servers_dock_->setFeature(ads::CDockWidget::DockWidgetClosable, true);
    reg_servers_dock_->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, false);
    reg_servers_dock_->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    reg_servers_dock_->setMinimumSize(180, 150);
    if (explorer_area != nullptr) {
        dock_manager_->addDockWidgetTabToArea(reg_servers_dock_, explorer_area);
    } else {
        dock_manager_->addDockWidget(ads::LeftDockWidgetArea, reg_servers_dock_);
    }
    reg_servers_dock_->toggleView(false);
    explorer_dock_->setAsCurrentTab();

    connect(dock_manager_, &ads::CDockManager::focusedDockWidgetChanged,
            this, [this](ads::CDockWidget*, ads::CDockWidget*) {
        refresh_commands();
    });

    update_workspace_area_style();
}

void MainWindow::connect_shortcuts() {
    shortcut_router_ = new ShortcutRouter(this, explorer_panel_, this);
    qApp->installEventFilter(shortcut_router_);

    connect(shortcut_router_, &ShortcutRouter::new_query_requested, this, &MainWindow::new_query_in_context);
    connect(shortcut_router_, &ShortcutRouter::close_active_tab_requested, this, &MainWindow::close_active_query);
    connect(shortcut_router_, &ShortcutRouter::show_explorer_requested, this, &MainWindow::show_object_explorer);
    connect(shortcut_router_, &ShortcutRouter::toggle_results_pane_requested, this, [this]() {
        if (auto* q = active_query()) {
            q->toggle_results_pane();
        }
    });
    connect(shortcut_router_, &ShortcutRouter::refresh_explorer_requested, explorer_panel_, &ObjectExplorerPanel::refresh_selected);
    connect(shortcut_router_, &ShortcutRouter::cancel_explorer_load_requested, explorer_panel_, &ObjectExplorerPanel::stop_selected_load);
    connect(shortcut_router_, &ShortcutRouter::execute_requested, this, [this]() {
        if (auto* q = active_query(); q != nullptr && q->is_connected() && !q->is_busy()) {
            emit q->execute_requested();
            q->execute_query();
        } else if (active_query() != nullptr && !active_query()->is_connected()) {
            status_label_->setText(QStringLiteral("Connect this query before executing."));
        }
    });
    connect(shortcut_router_, &ShortcutRouter::cancel_requested, this, [this]() {
        if (auto* q = active_query(); q != nullptr && q->is_busy()) {
            emit q->cancel_requested();
            q->cancel_query();
        }
    });
    connect(shortcut_router_, &ShortcutRouter::comment_selection_requested, this, [this]() {
        if (auto* q = active_query()) {
            q->comment_selection(false);
        }
    });
    connect(shortcut_router_, &ShortcutRouter::uncomment_selection_requested, this, [this]() {
        if (auto* q = active_query()) {
            q->comment_selection(true);
        }
    });
    connect(shortcut_router_, &ShortcutRouter::toggle_intellisense_requested, this, [this]() {
        if (auto* q = active_query()) {
            q->set_intellisense_enabled(!q->intellisense_enabled());
            refresh_commands();
            status_label_->setText(q->intellisense_enabled()
                ? QStringLiteral("IntelliSense enabled.")
                : QStringLiteral("IntelliSense disabled."));
        }
    });
    connect(shortcut_router_, &ShortcutRouter::status_hint, this, [this](const QString& text) {
        status_label_->setText(text);
    });
}

ads::CDockAreaWidget* MainWindow::find_document_area() const {
    if (workspace_dock_ != nullptr && workspace_dock_->dockAreaWidget() != nullptr) {
        return workspace_dock_->dockAreaWidget();
    }
    if (!query_docks_.empty()) {
        for (ads::CDockWidget* dock : query_docks_) {
            if (dock != nullptr && dock->dockAreaWidget() != nullptr && !dock->isInFloatingContainer()) {
                return dock->dockAreaWidget();
            }
        }
    }
    return nullptr;
}

QueryTabWidget* MainWindow::new_query(const QString& explicit_title, const QString& initial_text) {
    const capybaradb::core::EnvironmentDocument env_doc =
        capybaradb::core::load_environment_options(state_dir_ / "environment.json");

    auto* query = new QueryTabWidget(env_doc.normalized_default_results(), this);
    query->set_worker_pool(&worker_pool_);
    query->set_query_options(env_doc.normalized_query_options());
    if (!initial_text.isEmpty()) {
        query->set_editor_text(initial_text);
        query->set_dirty(false);
    }

    const QString title = explicit_title.isEmpty()
        ? QStringLiteral("SQLQuery%1.sql").arg(++query_sequence_)
        : explicit_title;

    auto* dock = new ads::CDockWidget(title);
    dock->setObjectName(QStringLiteral("query_%1").arg(query_sequence_));
    dock->setWidget(query, ads::CDockWidget::ForceNoScrollArea);
    dock->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    dock->setMinimumSize(200, 150);
    dock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
    dock->setFeature(ads::CDockWidget::DockWidgetPinnable, false);
    dock->setFeature(ads::CDockWidget::CustomCloseHandling, true);

    query_entries_.push_back(QueryEntry{query, dock, title});
    query_docks_.push_back(dock);

    connect(query, &QueryTabWidget::state_changed, this, [this, query]() {
        update_tab_title(query);
        refresh_commands();
    });
    connect(query, &QueryTabWidget::connection_changed, this, [this](bool) {
        refresh_commands();
    });
    connect(query, &QueryTabWidget::status_hint, status_label_, &QLabel::setText);
    connect(query, &QueryTabWidget::open_json_document_requested, this, &MainWindow::open_json_tab);
    connect(query, &QueryTabWidget::show_plan_in_new_tab_requested,
            this, [this](const std::vector<core::ExecutionPlanStatement>& stmts) {
        (void)open_plan_tab(stmts);
    });
    connect(query, &QueryTabWidget::open_script_in_new_query_requested,
            this, [this, query](const QString& sql) {
        if (QueryTabWidget* created = new_query(QString{}, sql)) {
            if (query->connection_config().has_value()) {
                created->connect_async(*query->connection_config(), &worker_pool_);
            }
        }
    });
    connect(query, &QueryTabWidget::query_executed, this, &MainWindow::record_query_history);
    connect(query, &QueryTabWidget::dirty_checkpoint_requested, this, [this, query]() {
        checkpoint_query_draft(query, false);
    });

    connect(dock, &ads::CDockWidget::closeRequested, this, [this, dock, query]() {
        if (confirm_close_tab(query)) {
            remove_query_draft(query);
            dock->closeDockWidget();
        }
    });
    connect(dock, &ads::CDockWidget::closed, this, [this, dock, query]() {
        std::erase_if(query_entries_, [query](const QueryEntry& e) { return e.widget == query; });
        std::erase(query_docks_, dock);
        update_workspace_area_style();
        refresh_commands();
    });

    if (ads::CDockAreaWidget* area = find_document_area()) {
        dock_manager_->addDockWidgetTabToArea(dock, area);
    } else if (explorer_dock_ != nullptr && explorer_dock_->dockAreaWidget() != nullptr) {
        dock_manager_->addDockWidget(ads::RightDockWidgetArea, dock, explorer_dock_->dockAreaWidget());
        apply_explorer_splitter_width(preferred_explorer_width_);
    } else {
        dock_manager_->addDockWidget(ads::CenterDockWidgetArea, dock);
    }

    dock->setAsCurrentTab();
    dock_manager_->setDockWidgetFocused(dock);
    QTimer::singleShot(0, query, [query]() {
        query->focus_editor();
    });
    update_workspace_area_style();
    refresh_commands();
    return query;
}

QueryTabWidget* MainWindow::open_json_tab(const QString& pretty_json) {
    ++json_sequence_;
    const QString title = QStringLiteral("JSON%1.json").arg(json_sequence_);
    QueryTabWidget* tab = new_query(title, pretty_json);
    if (tab != nullptr) {
        tab->editor_widget()->set_highlighting_enabled(false);
        tab->set_dirty(false);
        update_tab_title(tab);
    }
    return tab;
}

ExecutionPlanWidget* MainWindow::open_plan_tab(
    std::vector<capybaradb::core::ExecutionPlanStatement> statements,
    const QString& explicit_title) {
    auto* viewer = new ExecutionPlanWidget(false, this);
    viewer->set_statements(std::move(statements));
    connect(viewer, &ExecutionPlanWidget::open_script_in_new_query_requested, this, [this](const QString& sql) {
        std::optional<drivers::ConnectionConfig> inherited;
        if (QueryTabWidget* cur = active_query(); cur != nullptr && cur->connection_config().has_value()) {
            inherited = cur->connection_config();
        } else if (explorer_panel_ != nullptr) {
            inherited = explorer_panel_->primary_connection();
        }
        if (QueryTabWidget* created = new_query(QString{}, sql); created != nullptr && inherited.has_value()) {
            created->connect_async(*inherited, &worker_pool_);
        }
    });
    connect(viewer, &ExecutionPlanWidget::status_message, status_label_, &QLabel::setText);

    const QString title = explicit_title.isEmpty()
        ? QStringLiteral("Plan%1.plan").arg(++plan_sequence_)
        : explicit_title;
    auto* dock = new ads::CDockWidget(title);
    dock->setObjectName(QStringLiteral("plan_%1").arg(plan_sequence_));
    dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
    dock->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    dock->setMinimumSize(240, 180);
    dock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
    dock->setFeature(ads::CDockWidget::DockWidgetPinnable, false);

    plan_docks_.push_back(dock);
    connect(dock, &ads::CDockWidget::closed, this, [this, dock]() {
        std::erase(plan_docks_, dock);
        update_workspace_area_style();
        refresh_commands();
    });

    if (ads::CDockAreaWidget* area = find_document_area()) {
        dock_manager_->addDockWidgetTabToArea(dock, area);
    } else if (explorer_dock_ != nullptr && explorer_dock_->dockAreaWidget() != nullptr) {
        dock_manager_->addDockWidget(ads::RightDockWidgetArea, dock, explorer_dock_->dockAreaWidget());
        apply_explorer_splitter_width(preferred_explorer_width_);
    } else {
        dock_manager_->addDockWidget(ads::CenterDockWidgetArea, dock);
    }

    dock->setAsCurrentTab();
    dock_manager_->setDockWidgetFocused(dock);
    update_workspace_area_style();
    refresh_commands();
    return viewer;
}

void MainWindow::open_execution_plan_file() {
    QString path;
    if (open_plan_file_picker_) {
        path = open_plan_file_picker_();
    } else {
        path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Open Execution Plan"),
            QString{},
            QStringLiteral("Execution Plans (*.sqlplan *.json *.xml);;All Files (*.*)"));
    }
    if (path.trimmed().isEmpty()) {
        return;
    }

    const core::EnvironmentDocument env_doc =
        core::load_environment_options(state_dir_ / "environment.json");
    auto loaded = core::load_execution_plan_file(
        std::filesystem::path(path.toStdWString()),
        env_doc.normalized_query_options().plan_warnings());
    if (!loaded.has_value()) {
        const QString err = QString::fromStdString(loaded.error());
        status_label_->setText(err);
        QMessageBox::warning(this, QStringLiteral("Open Execution Plan"), err);
        return;
    }

    const QFileInfo fi(path);
    (void)open_plan_tab(std::move(*loaded), fi.fileName());
    status_label_->setText(QStringLiteral("Opened execution plan %1.").arg(fi.fileName()));
}

bool MainWindow::close_active_query() {
    QueryTabWidget* active = active_query();
    if (active == nullptr) {
        return false;
    }
    if (!confirm_close_tab(active)) {
        return false;
    }
    remove_query_draft(active);
    for (const QueryEntry& e : query_entries_) {
        if (e.widget == active && e.dock != nullptr) {
            e.dock->closeDockWidget();
            return true;
        }
    }
    return false;
}

QueryTabWidget* MainWindow::active_query() const {
    if (dock_manager_ != nullptr) {
        if (ads::CDockWidget* focused = dock_manager_->focusedDockWidget()) {
            for (const QueryEntry& e : query_entries_) {
                if (e.dock == focused) {
                    return e.widget;
                }
            }
        }
    }
    for (const QueryEntry& e : query_entries_) {
        if (e.dock != nullptr && e.dock->isCurrentTab() && !e.dock->isClosed()) {
            return e.widget;
        }
    }
    return query_entries_.empty() ? nullptr : query_entries_.back().widget;
}

QString MainWindow::active_tab_title() const {
    if (QueryTabWidget* q = active_query()) {
        for (const QueryEntry& e : query_entries_) {
            if (e.widget == q && e.dock != nullptr) {
                return e.dock->windowTitle();
            }
        }
    }
    return {};
}

void MainWindow::update_tab_title(QueryTabWidget* query) {
    for (const QueryEntry& e : query_entries_) {
        if (e.widget == query && e.dock != nullptr) {
            e.dock->setWindowTitle(e.base_title + (query->is_dirty() ? QStringLiteral("*") : QString{}));
            break;
        }
    }
}

void MainWindow::show_object_explorer() {
    if (explorer_dock_ == nullptr) {
        return;
    }
    if (explorer_dock_->isAutoHide()) {
        explorer_dock_->setAutoHide(false);
    }
    explorer_dock_->toggleView(true);
    explorer_dock_->setAsCurrentTab();
    dock_manager_->setDockWidgetFocused(explorer_dock_);
    explorer_panel_->focus_tree();
}

void MainWindow::show_registered_servers() {
    if (reg_servers_dock_ == nullptr || reg_servers_panel_ == nullptr) {
        return;
    }
    reg_servers_panel_->refresh();
    if (reg_servers_dock_->isAutoHide()) {
        reg_servers_dock_->setAutoHide(false);
    }
    reg_servers_dock_->toggleView(true);
    reg_servers_dock_->setAsCurrentTab();
    dock_manager_->setDockWidgetFocused(reg_servers_dock_);
}

void MainWindow::show_query_history_dialog() {
    QueryHistoryDialog dlg(state_dir_, this);
    connect(&dlg, &QueryHistoryDialog::open_in_new_query_requested,
            this, &MainWindow::open_history_entry_in_new_query);
    dlg.exec();
}

void MainWindow::open_query_options_dialog() {
    QueryTabWidget* q = active_query();
    if (q == nullptr) {
        return;
    }
    QueryOptionsDialog dlg(q->query_options(), this);
    if (dlg.exec() == QDialog::Accepted) {
        q->set_query_options(dlg.options());
        status_label_->setText(QStringLiteral("Query options updated for the active tab."));
        refresh_commands();
    }
}

void MainWindow::open_environment_options_dialog() {
    OptionsDialog dlg(state_dir_, this);
    if (dlg.exec() == QDialog::Accepted) {
        status_label_->setText(QStringLiteral("Environment options saved."));
    }
}

void MainWindow::open_history_entry_in_new_query(
    const QString& sql,
    const QString& engine,
    const QString& server,
    int port,
    const QString& database,
    const QString& user) {
    QueryTabWidget* tab = new_query(QString{}, sql);
    if (tab == nullptr) {
        return;
    }
    const drivers::EngineKind want_engine =
        (engine.compare(QStringLiteral("SqlServer"), Qt::CaseInsensitive) == 0)
            ? drivers::EngineKind::SqlServer
            : drivers::EngineKind::PostgreSql;
    tab->editor_widget()->set_engine_dialect(want_engine);
    tab->set_dirty(false);

    // Connect to matching open session if available; never execute automatically.
    std::optional<drivers::ConnectionConfig> match;
    auto matches_cfg = [&](const drivers::ConnectionConfig& c) {
        return c.engine == want_engine &&
               QString::fromStdString(c.host).compare(server, Qt::CaseInsensitive) == 0 &&
               static_cast<int>(c.port) == port &&
               QString::fromStdString(c.database).compare(database, Qt::CaseInsensitive) == 0 &&
               (user.isEmpty() || QString::fromStdString(c.username).compare(user, Qt::CaseInsensitive) == 0);
    };
    for (const QueryEntry& e : query_entries_) {
        if (e.widget != nullptr && e.widget != tab && e.widget->connection_config().has_value() &&
            matches_cfg(*e.widget->connection_config())) {
            match = e.widget->connection_config();
            break;
        }
    }
    if (!match.has_value() && explorer_panel_ != nullptr) {
        if (auto p = explorer_panel_->primary_connection(); p.has_value() && matches_cfg(*p)) {
            match = p;
        }
    }
    if (match.has_value()) {
        tab->connect_async(*match, &worker_pool_);
    }
}

void MainWindow::record_query_history(const core::QueryHistoryCapture& capture) {
    const core::EnvironmentDocument env_doc =
        core::load_environment_options(state_dir_ / "environment.json");
    if (!env_doc.query_history.enabled) {
        return;
    }
    if (core::is_query_history_disabled_for_connection(
            state_dir_ / "connections.json",
            capture.engine,
            capture.server,
            capture.port,
            capture.database)) {
        return;
    }

    const std::filesystem::path history_path = state_dir_ / "query-history.dat";
    const core::QueryHistorySettings settings = env_doc.query_history.normalized();
    worker_pool_.submit([history_path, capture, settings]() {
        (void)core::record_query_history_entry(history_path, capture, settings);
    });
}

void MainWindow::checkpoint_query_draft(QueryTabWidget* tab, bool sync) {
    if (tab == nullptr || !tab->is_dirty() || !tab->file_path().isEmpty() || tab->draft_id().empty()) {
        return;
    }
    QString base_title = QStringLiteral("SQLQuery.sql");
    for (const QueryEntry& e : query_entries_) {
        if (e.widget == tab) {
            base_title = e.base_title;
            break;
        }
    }

    core::RecoveryDraft draft;
    draft.id                      = tab->draft_id();
    draft.title                   = base_title.toStdString();
    draft.text                    = tab->editor_text().toStdString();
    draft.engine                  = (tab->connection_config().has_value() &&
                                     tab->connection_config()->engine == drivers::EngineKind::SqlServer)
                                        ? "SqlServer"
                                        : "PostgreSql";
    draft.command_timeout_seconds = tab->query_options().execution_timeout_seconds;
    draft.word_wrap               = tab->word_wrap();
    draft.show_white_space        = tab->show_white_space();
    draft.updated_utc_ms          = core::current_utc_ms();

    const std::filesystem::path rec_dir = state_dir_ / "recovery";
    if (sync) {
        (void)core::save_recovery_draft(rec_dir, draft);
    } else {
        worker_pool_.submit([rec_dir, draft = std::move(draft)]() {
            (void)core::save_recovery_draft(rec_dir, draft);
        });
    }
}

void MainWindow::checkpoint_all_drafts_sync() {
    for (const QueryEntry& e : query_entries_) {
        checkpoint_query_draft(e.widget, true);
    }
}

void MainWindow::remove_query_draft(QueryTabWidget* tab) {
    if (tab == nullptr || tab->draft_id().empty()) {
        return;
    }
    (void)core::remove_recovery_draft(state_dir_ / "recovery", tab->draft_id());
}

bool MainWindow::check_draft_recovery() {
    const std::filesystem::path rec_dir = state_dir_ / "recovery";
    const std::vector<std::string> ids = core::list_recovery_draft_ids(rec_dir);
    if (ids.empty()) {
        return false;
    }

    const int count = static_cast<int>(ids.size());
    const bool recover = recovery_prompt_
        ? recovery_prompt_(count)
        : (QMessageBox::question(
               this,
               QStringLiteral("Recover Unsaved Queries"),
               QStringLiteral("CapybaraDB found %1 unsaved query draft(s) from a previous session.\n\nRecover them?")
                   .arg(count),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::Yes) == QMessageBox::Yes);

    if (!recover) {
        core::clear_all_recovery_drafts(rec_dir);
        return false;
    }

    int restored = 0;
    for (const std::string& id : ids) {
        auto draft_res = core::read_recovery_draft(rec_dir, id);
        if (!draft_res.has_value()) {
            (void)core::remove_recovery_draft(rec_dir, id);
            continue;
        }
        const core::RecoveryDraft& d = *draft_res;
        const QString title = d.title.empty() ? QStringLiteral("Recovered.sql") : QString::fromStdString(d.title);
        QueryTabWidget* tab = new_query(title, QString::fromStdString(d.text));
        if (tab != nullptr) {
            tab->set_draft_id(d.id);
            core::QueryOptions qopts = tab->query_options();
            qopts.execution_timeout_seconds = d.command_timeout_seconds;
            tab->set_query_options(qopts);
            tab->set_word_wrap(d.word_wrap);
            tab->set_show_white_space(d.show_white_space);
            tab->editor_widget()->set_engine_dialect(
                d.engine == "SqlServer" ? drivers::EngineKind::SqlServer : drivers::EngineKind::PostgreSql);
            tab->set_dirty(true);
            update_tab_title(tab);
            ++restored;
        }
    }
    if (restored > 0) {
        status_label_->setText(QStringLiteral("Recovered %1 unsaved query draft(s).").arg(restored));
        return true;
    }
    return false;
}

void MainWindow::apply_explorer_splitter_width(int width_px) {
    if (explorer_dock_ == nullptr || explorer_dock_->dockAreaWidget() == nullptr || dock_manager_ == nullptr) {
        return;
    }
    ads::CDockAreaWidget* explorer_area = explorer_dock_->dockAreaWidget();
    auto* splitter = explorer_area->parentSplitter();
    if (splitter == nullptr || splitter->count() < 2) {
        return;
    }

    const int total_w = std::max({splitter->width(), dock_manager_->width(), width(), theme::kMinWindowWidthPx});
    const int max_left = std::min(600, std::max(180, total_w - 240));
    const int clamped_left = std::clamp(width_px, 180, max_left);
    const int exp_idx = splitter->indexOf(explorer_area);
    if (exp_idx < 0) {
        return;
    }

    for (int i = 0; i < splitter->count(); ++i) {
        splitter->setStretchFactor(i, i == exp_idx ? 0 : 1);
    }
    if (splitter->count() == 2) {
        const int remaining = std::max(240, total_w - clamped_left);
        const QList<int> sizes = (exp_idx == 0)
            ? QList<int>{clamped_left, remaining}
            : QList<int>{remaining, clamped_left};
        dock_manager_->setSplitterSizes(explorer_area, sizes);
    }
}

void MainWindow::update_workspace_area_style() {
    if (explorer_dock_ != nullptr && explorer_dock_->dockAreaWidget() != nullptr) {
        ads::CDockAreaWidget* exp_area = explorer_dock_->dockAreaWidget();
        if (exp_area->objectName() != QStringLiteral("ExplorerDockArea")) {
            exp_area->setObjectName(QStringLiteral("ExplorerDockArea"));
            exp_area->style()->unpolish(exp_area);
            exp_area->style()->polish(exp_area);
        }
    }

    ads::CDockAreaWidget* area = (workspace_dock_ != nullptr) ? workspace_dock_->dockAreaWidget() : nullptr;
    if (area == nullptr) {
        return;
    }
    const bool is_empty = (area->openDockWidgetsCount() <= 1);
    area->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
    area->setProperty("emptyWorkspace", is_empty);
    if (ads::CDockAreaTitleBar* tb = area->titleBar()) {
        tb->setVisible(!is_empty && !area->isAutoHide());
    }
    area->style()->unpolish(area);
    area->style()->polish(area);
    area->update();
}

void MainWindow::reset_window_layout() {
    if (dock_manager_ == nullptr || explorer_dock_ == nullptr) {
        return;
    }

    QueryTabWidget* prev_active = active_query();
    preferred_explorer_width_ = theme::kObjectExplorerInitialWidthPx;

    if (explorer_dock_->isAutoHide()) {
        explorer_dock_->setAutoHide(false);
    }

    // Detach explorer, registered servers, query and plan docks without deleting open tabs.
    // Keep workspace_dock_ intact as ADS's permanent central widget.
    const bool reg_was_open = (reg_servers_dock_ != nullptr && !reg_servers_dock_->isClosed());
    if (reg_servers_dock_ != nullptr) {
        if (reg_servers_dock_->isAutoHide()) {
            reg_servers_dock_->setAutoHide(false);
        }
        dock_manager_->removeDockWidget(reg_servers_dock_);
    }
    dock_manager_->removeDockWidget(explorer_dock_);
    for (ads::CDockWidget* dock : query_docks_) {
        dock_manager_->removeDockWidget(dock);
    }
    for (ads::CDockWidget* dock : plan_docks_) {
        dock_manager_->removeDockWidget(dock);
    }

    explorer_dock_->setFeature(ads::CDockWidget::DockWidgetClosable, false);
    ads::CDockAreaWidget* left_area = dock_manager_->addDockWidget(ads::LeftDockWidgetArea, explorer_dock_);
    if (left_area != nullptr) {
        left_area->setObjectName(QStringLiteral("ExplorerDockArea"));
    }
    if (reg_servers_dock_ != nullptr) {
        if (left_area != nullptr) {
            dock_manager_->addDockWidgetTabToArea(reg_servers_dock_, left_area);
        } else {
            dock_manager_->addDockWidget(ads::LeftDockWidgetArea, reg_servers_dock_);
        }
        reg_servers_dock_->toggleView(reg_was_open);
    }
    explorer_dock_->toggleView(true);
    explorer_dock_->setAsCurrentTab();

    ads::CDockAreaWidget* doc_area = (workspace_dock_ != nullptr) ? workspace_dock_->dockAreaWidget() : nullptr;
    for (ads::CDockWidget* dock : query_docks_) {
        if (doc_area == nullptr) {
            doc_area = dock_manager_->addDockWidget(ads::RightDockWidgetArea, dock, left_area);
        } else {
            dock_manager_->addDockWidgetTabToArea(dock, doc_area);
        }
        dock->toggleView(true);
    }
    for (ads::CDockWidget* dock : plan_docks_) {
        if (doc_area == nullptr) {
            doc_area = dock_manager_->addDockWidget(ads::RightDockWidgetArea, dock, left_area);
        } else {
            dock_manager_->addDockWidgetTabToArea(dock, doc_area);
        }
        dock->toggleView(true);
    }

    apply_explorer_splitter_width(preferred_explorer_width_);
    update_workspace_area_style();

    if (prev_active != nullptr) {
        for (const QueryEntry& e : query_entries_) {
            if (e.widget == prev_active && e.dock != nullptr) {
                e.dock->setAsCurrentTab();
                dock_manager_->setDockWidgetFocused(e.dock);
                break;
            }
        }
    } else if (workspace_dock_ != nullptr && workspace_dock_->dockAreaWidget() != nullptr) {
        workspace_dock_->dockAreaWidget()->setCurrentDockWidget(workspace_dock_);
    }
    refresh_commands();
}

void MainWindow::set_results_destination(capybaradb::core::ResultsDestination dest) {
    if (QueryTabWidget* q = active_query()) {
        q->set_results_destination(dest);
    }
    refresh_commands();
}

void MainWindow::refresh_commands() {
    QueryTabWidget* q = active_query();
    const bool has_tab   = (q != nullptr);
    const bool connected = (q != nullptr && q->is_connected());
    const bool busy      = (q != nullptr && q->is_busy());

    act_conn_connect_->setEnabled(!has_tab || (!connected && !busy));
    act_conn_disconnect_->setEnabled(connected && !busy);
    act_conn_change_->setEnabled(connected && !busy);

    act_execute_->setEnabled(connected && !busy);
    act_execute_->setIcon(get_icon(act_execute_->isEnabled() ? IconId::Execute : IconId::ExecuteDisabled));

    act_cancel_->setEnabled(busy && !q->is_canceling());
    act_cancel_->setIcon(get_icon(act_cancel_->isEnabled() ? IconId::Cancel : IconId::CancelDisabled));

    db_combo_->setEnabled(connected && !busy);
    db_combo_->setToolTip(has_tab
        ? QStringLiteral("Database of the active query session. Choosing another one opens a new connection for this tab: SET values and temporary tables do not carry over.")
        : QStringLiteral("Activate a query tab to see its database and session status. Object Explorer uses a separate connection."));
    if (!connected) {
        const QString shown = has_tab ? QStringLiteral("Not connected") : QStringLiteral("No active query");
        if (db_combo_->count() != 1 || db_combo_->currentText() != shown) {
            db_combo_->clear();
            db_combo_->addItem(shown);
            db_combo_->setCurrentIndex(0);
        }
    } else {
        const QString cur_db = QString::fromStdString(q->session_info().database);
        const int idx = db_combo_->findText(cur_db);
        if (idx >= 0) {
            db_combo_->setCurrentIndex(idx);
        } else {
            db_combo_->clear();
            db_combo_->addItem(cur_db);
            db_combo_->setCurrentIndex(0);
        }
    }
    act_est_plan_->setEnabled(connected && !busy);
    act_parse_->setEnabled(connected && !busy);
    act_open_in_explorer_->setEnabled(connected);

    act_actual_plan_->setEnabled(has_tab);
    act_actual_plan_->setChecked(has_tab && q->include_actual_plan());

    act_client_stats_->setEnabled(has_tab);
    act_client_stats_->setChecked(has_tab && q->include_client_statistics());
    act_reset_stats_->setEnabled(has_tab);

    act_intellisense_->setEnabled(has_tab);
    act_intellisense_->setChecked(has_tab && q->intellisense_enabled());
    act_list_members_->setEnabled(has_tab);
    act_complete_word_->setEnabled(has_tab);
    act_refresh_cache_->setEnabled(has_tab);

    act_save_->setEnabled(has_tab);
    act_save_as_->setEnabled(has_tab);
    act_close_tab_->setEnabled(has_tab);
    act_undo_->setEnabled(has_tab);
    act_redo_->setEnabled(has_tab);
    act_cut_->setEnabled(has_tab);
    act_copy_->setEnabled(has_tab);
    act_paste_->setEnabled(has_tab);
    act_find_->setEnabled(has_tab);
    act_go_to_line_->setEnabled(has_tab);
    act_replace_->setEnabled(has_tab);
    act_comment_->setEnabled(has_tab);
    act_uncomment_->setEnabled(has_tab);

    act_word_wrap_->setEnabled(has_tab);
    act_word_wrap_->setChecked(has_tab && q->word_wrap());
    act_white_space_->setEnabled(has_tab);
    act_white_space_->setChecked(has_tab && q->show_white_space());

    act_results_text_->setEnabled(has_tab);
    act_results_grid_->setEnabled(has_tab);
    act_results_file_->setEnabled(has_tab);
    act_query_options_->setEnabled(has_tab);
    if (has_tab) {
        const auto mode = q->results_destination();
        act_results_text_->setChecked(mode == capybaradb::core::ResultsDestination::Text);
        act_results_grid_->setChecked(mode == capybaradb::core::ResultsDestination::Grid);
        act_results_file_->setChecked(mode == capybaradb::core::ResultsDestination::File);
    } else {
        act_results_group_->setExclusive(false);
        act_results_text_->setChecked(false);
        act_results_grid_->setChecked(false);
        act_results_file_->setChecked(false);
        act_results_group_->setExclusive(true);
    }
}

void MainWindow::apply_default_window_geometry() {
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary == nullptr) {
        setMinimumSize(theme::kMinWindowWidthPx, theme::kMinWindowHeightPx);
        resize(theme::kDefaultWindowWidthPx, theme::kDefaultWindowHeightPx);
        return;
    }

    const QRect avail = primary->availableGeometry();
    const int min_w = std::min(theme::kMinWindowWidthPx, std::max(200, avail.width() - 16));
    const int min_h = std::min(theme::kMinWindowHeightPx, std::max(150, avail.height() - 16));
    setMinimumSize(min_w, min_h);

    const int desired_w = std::min(theme::kDefaultWindowWidthPx, std::max(min_w, avail.width() - 16));
    const int desired_h = std::min(theme::kDefaultWindowHeightPx, std::max(min_h, avail.height() - 16));
    const int x = avail.x() + (avail.width() - desired_w) / 2;
    const int y = avail.y() + (avail.height() - desired_h) / 2;
    const capybaradb::core::ScreenRect fitted = capybaradb::core::fit_bounds_to_work_area(
        capybaradb::core::ScreenRect{x, y, desired_w, desired_h},
        to_core_rect(avail));
    setGeometry(fitted.left, fitted.top, fitted.width, fitted.height);
}

void MainWindow::restore_window_placement() {
    const capybaradb::core::EnvironmentDocument doc =
        capybaradb::core::load_environment_options(state_dir_ / "environment.json");
    if (!doc.restore_window_placement || !doc.window.has_value()) {
        return;
    }

    std::vector<capybaradb::core::ScreenRect> screens;
    for (QScreen* s : QGuiApplication::screens()) {
        if (s != nullptr) {
            screens.push_back(to_core_rect(s->availableGeometry()));
        }
    }
    if (!capybaradb::core::is_placement_visible_on_screens(*doc.window, screens)) {
        return;
    }

    const capybaradb::core::WindowPlacement& wp = *doc.window;
    preferred_explorer_width_ = doc.normalized_explorer_width();
    setGeometry(wp.left, wp.top, wp.width, wp.height);
    if (wp.maximized) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void MainWindow::restore_dock_layout() {
    const std::filesystem::path layout_file = state_dir_ / "layout.bin";
    std::error_code ec;
    if (!std::filesystem::exists(layout_file, ec) || ec) {
        reset_window_layout();
        return;
    }

    QFile file(QString::fromStdWString(layout_file.wstring()));
    if (!file.open(QIODevice::ReadOnly)) {
        reset_window_layout();
        return;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty() || !dock_manager_->restoreState(bytes)) {
        status_label_->setText(QStringLiteral("Saved layout could not be restored. Using the default layout."));
        reset_window_layout();
        return;
    }
    apply_explorer_splitter_width(preferred_explorer_width_);
    update_workspace_area_style();
}

void MainWindow::restore_persistent_state() {
    restore_window_placement();
    restore_dock_layout();
}

void MainWindow::save_persistent_state() {
    const std::filesystem::path env_path = state_dir_ / "environment.json";
    const QRect bounds = isMaximized() ? normalGeometry() : geometry();
    const int exp_width = (explorer_dock_ != nullptr && explorer_dock_->dockAreaWidget() != nullptr
                           && explorer_dock_->dockAreaWidget()->isVisible())
        ? std::clamp(explorer_dock_->dockAreaWidget()->width(), 180, 600)
        : preferred_explorer_width_;

    if (bounds.isValid() && bounds.width() > 0 && bounds.height() > 0) {
        capybaradb::core::EnvironmentDocument doc = capybaradb::core::load_environment_options(env_path);
        if (doc.restore_window_placement) {
            doc.window = capybaradb::core::WindowPlacement{
                bounds.x(),
                bounds.y(),
                bounds.width(),
                bounds.height(),
                isMaximized(),
                exp_width,
            };
            (void)capybaradb::core::save_environment_options(env_path, doc);
        }
    }

    if (dock_manager_ != nullptr) {
        std::error_code ec;
        std::filesystem::create_directories(state_dir_, ec);
        const std::filesystem::path layout_file = state_dir_ / "layout.bin";
        QSaveFile out(QString::fromStdWString(layout_file.wstring()));
        if (out.open(QIODevice::WriteOnly)) {
            out.write(dock_manager_->saveState());
            out.commit();
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    const std::vector<QueryEntry> entries = query_entries_;
    for (const QueryEntry& entry : entries) {
        if (entry.widget != nullptr && !confirm_close_tab(entry.widget)) {
            event->ignore();
            return;
        }
    }
    for (const QueryEntry& entry : entries) {
        remove_query_draft(entry.widget);
    }
    save_persistent_state();
    QMainWindow::closeEvent(event);
}

void MainWindow::open_files_dialog() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Open SQL File"),
        QString{},
        QStringLiteral("SQL files (*.sql);;JSON files (*.json);;All files (*.*)"));
    if (paths.isEmpty()) {
        return;
    }

    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("Open File"), file.errorString());
            continue;
        }
        const QString text = QString::fromUtf8(file.readAll());
        const QFileInfo fi(path);
        if (QueryTabWidget* q = new_query(fi.fileName(), text)) {
            q->set_file_path(fi.absoluteFilePath());
            q->editor_widget()->set_highlighting_enabled(!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive));
            q->set_dirty(false);
            if (auto inherited = explorer_panel_->selected_connection(); inherited.has_value()) {
                q->connect_async(*inherited, &worker_pool_);
            } else if (auto primary = explorer_panel_->primary_connection(); primary.has_value()) {
                q->connect_async(*primary, &worker_pool_);
            }
        }
    }
}

bool MainWindow::save_query_tab(QueryTabWidget* tab, bool save_as) {
    if (tab == nullptr) {
        return false;
    }
    QString path = tab->file_path();
    if (save_as || path.isEmpty()) {
        QString suggested = active_tab_title();
        if (suggested.endsWith(QLatin1Char('*'))) {
            suggested.chop(1);
        }
        path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Save Query"),
            path.isEmpty() ? suggested : path,
            QStringLiteral("SQL files (*.sql);;All files (*.*)"));
        if (path.isEmpty()) {
            return false;
        }
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Save Query"), out.errorString());
        return false;
    }
    const QByteArray bytes = tab->editor_text().toUtf8();
    out.write(bytes);
    if (!out.commit()) {
        QMessageBox::warning(this, QStringLiteral("Save Query"), out.errorString());
        return false;
    }

    const QFileInfo fi(path);
    tab->set_file_path(fi.absoluteFilePath());
    for (QueryEntry& e : query_entries_) {
        if (e.widget == tab) {
            e.base_title = fi.fileName();
            break;
        }
    }
    tab->set_dirty(false);
    remove_query_draft(tab);
    update_tab_title(tab);
    status_label_->setText(QStringLiteral("Item saved"));
    return true;
}

bool MainWindow::confirm_close_tab(QueryTabWidget* tab) {
    if (tab == nullptr) {
        return true;
    }
    if (tab->is_busy()) {
        QMessageBox::information(
            this,
            QStringLiteral("CapybaraDB"),
            QStringLiteral("Cancel the running operation before closing this query."));
        return false;
    }

    if (tab->is_dirty()) {
        const QMessageBox::StandardButton ans = dirty_close_prompt_
            ? dirty_close_prompt_(tab)
            : QMessageBox::question(
                  this,
                  QStringLiteral("CapybaraDB"),
                  QStringLiteral("Save changes to this query?"),
                  QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                  QMessageBox::Yes);
        if (ans == QMessageBox::Cancel) {
            return false;
        }
        if (ans == QMessageBox::Yes) {
            if (!save_query_tab(tab, false) || tab->is_dirty()) {
                return false;
            }
        }
    }

    const core::EnvironmentDocument env_doc =
        core::load_environment_options(state_dir_ / "environment.json");
    if (env_doc.check_open_transactions && tab->is_connected()) {
        const auto risk_res = tab->inspect_loss_risk_sync();
        if (risk_res.has_value() && risk_res->open_transaction) {
            const bool aborted = risk_res->aborted_transaction;
            const QMessageBox::StandardButton tx_ans = open_tx_close_prompt_
                ? open_tx_close_prompt_(aborted)
                : (aborted
                       ? QMessageBox::warning(
                             this,
                             QStringLiteral("CapybaraDB"),
                             QStringLiteral("The transaction in this query has failed and will be rolled back. Close the query?"),
                             QMessageBox::Ok | QMessageBox::Cancel,
                             QMessageBox::Cancel)
                       : QMessageBox::question(
                             this,
                             QStringLiteral("CapybaraDB"),
                             QStringLiteral("There are uncommitted transactions. Do you wish to commit these transactions?"),
                             QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                             QMessageBox::Yes));

            if (tx_ans == QMessageBox::Cancel) {
                return false;
            }
            if (!aborted && tx_ans == QMessageBox::Yes) {
                const auto commit_res = tab->commit_transaction_sync();
                if (!commit_res.has_value()) {
                    status_label_->setText(
                        QStringLiteral("Commit failed; query remains open: %1")
                            .arg(QString::fromStdString(commit_res.error().format_short())));
                    return false;
                }
            }
        }
    }
    return true;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!initial_show_done_) {
        initial_show_done_ = true;
        apply_explorer_splitter_width(preferred_explorer_width_);
        update_workspace_area_style();
        QTimer::singleShot(0, this, [this]() {
            apply_explorer_splitter_width(preferred_explorer_width_);
            update_workspace_area_style();
        });
    }
}

void MainWindow::show_about_dialog() {
    QMessageBox::about(
        this,
        QStringLiteral("About CapybaraDB"),
        QStringLiteral("CapybaraDB\nVersion 0.1.0"));
}

QueryTabWidget* MainWindow::new_query_in_context() {
    std::optional<drivers::ConnectionConfig> inherited;
    if (QueryTabWidget* cur = active_query(); cur != nullptr && cur->connection_config().has_value()) {
        inherited = cur->connection_config();
    } else if (explorer_panel_ != nullptr) {
        inherited = explorer_panel_->selected_connection();
        if (!inherited.has_value()) {
            inherited = explorer_panel_->primary_connection();
        }
    }

    QueryTabWidget* created = new_query();
    if (created != nullptr && inherited.has_value()) {
        created->connect_async(*inherited, &worker_pool_);
    }
    return created;
}

void MainWindow::prompt_connect_at_startup() {
    if (!isVisible() || QApplication::activeModalWidget() != nullptr) {
        return;
    }
    if (check_draft_recovery()) {
        return;
    }
    if (explorer_panel_ == nullptr || explorer_panel_->is_connected()) {
        return;
    }

    ConnectionDialog dlg(&worker_pool_, state_dir_, this);
    if (dlg.exec() == QDialog::Accepted) {
        connect_explorer_with_config(dlg.connection_config());
    }
}

void MainWindow::connect_with_dialog() {
    ConnectionDialog dlg(&worker_pool_, state_dir_, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const drivers::ConnectionConfig cfg = dlg.connection_config();
    QueryTabWidget* q = active_query();
    if (q == nullptr || q->is_connected() || q->is_busy()) {
        q = new_query();
    }

    status_label_->setText(QStringLiteral("Connecting…"));
    QPointer<MainWindow> self(this);
    q->connect_async(cfg, &worker_pool_, [self](bool ok, const QString& err) {
        if (!self) {
            return;
        }
        self->status_label_->setText(ok ? QStringLiteral("Connected") : err);
        self->refresh_commands();
    });
    connect_explorer_with_config(cfg);
}

void MainWindow::connect_explorer_with_config(const drivers::ConnectionConfig& cfg) {
    status_label_->setText(QStringLiteral("Connecting Object Explorer…"));
    auto token = std::make_shared<core::CancelToken>();
    QPointer<MainWindow> self(this);

    worker_pool_.submit([self, cfg, token]() {
        auto drv = drivers::create_db_driver(cfg.engine);
        auto res = drv->connect(cfg, *token);
        drivers::SessionInfo info;
        if (res.has_value()) {
            info = drv->session_info();
        }
        drv->disconnect();

        QMetaObject::invokeMethod(
            self,
            [self, cfg, info = std::move(info), res = std::move(res)]() {
                if (!self) {
                    return;
                }
                if (!res.has_value()) {
                    const QString msg = QString::fromStdString(res.error().format_short());
                    self->status_label_->setText(msg);
                    QMessageBox::warning(self, QStringLiteral("Connect to Server"), msg);
                    return;
                }
                self->explorer_panel_->set_connected_server(cfg, info);
                self->status_label_->setText(QStringLiteral("Connected"));
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::connect_active_query_with_dialog() {
    if (QueryTabWidget* cur = active_query(); cur != nullptr && (cur->is_connected() || cur->is_busy())) {
        return;
    }
    ConnectionDialog dlg(&worker_pool_, state_dir_, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QueryTabWidget* q = active_query();
    if (q == nullptr) {
        q = new_query();
    }
    status_label_->setText(QStringLiteral("Connecting…"));
    QPointer<MainWindow> self(this);
    q->connect_async(dlg.connection_config(), &worker_pool_, [self](bool ok, const QString& err) {
        if (!self) {
            return;
        }
        self->status_label_->setText(ok ? QStringLiteral("Connected") : err);
        self->refresh_commands();
    });
}

void MainWindow::change_active_query_connection() {
    QueryTabWidget* q = active_query();
    if (q == nullptr || !q->is_connected() || q->is_busy()) {
        return;
    }
    ConnectionDialog dlg(&worker_pool_, state_dir_, this);
    if (q->connection_config().has_value()) {
        dlg.prefill(*q->connection_config());
    }
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    status_label_->setText(QStringLiteral("Changing connection…"));
    QPointer<MainWindow> self(this);
    q->connect_async(dlg.connection_config(), &worker_pool_, [self](bool ok, const QString& err) {
        if (!self) {
            return;
        }
        self->status_label_->setText(ok ? QStringLiteral("Connected") : err);
        self->refresh_commands();
    });
}

void MainWindow::disconnect_active_query() {
    QueryTabWidget* q = active_query();
    if (q == nullptr || !q->is_connected() || q->is_busy()) {
        return;
    }
    q->disconnect_session();
    status_label_->setText(QStringLiteral("Disconnected"));
    refresh_commands();
}

void MainWindow::open_active_server_in_explorer() {
    QueryTabWidget* q = active_query();
    if (q == nullptr || !q->connection_config().has_value()) {
        return;
    }
    show_object_explorer();
    connect_explorer_with_config(*q->connection_config());
}

void MainWindow::populate_databases_popup() {
    QueryTabWidget* q = active_query();
    if (q == nullptr || !q->is_connected() || q->is_busy() || !q->connection_config().has_value()) {
        return;
    }

    if (db_list_cancel_) {
        db_list_cancel_->cancel();
    }
    db_list_cancel_ = std::make_shared<core::CancelToken>();
    const uint64_t gen = ++db_list_generation_;

    const drivers::ConnectionConfig cfg = *q->connection_config();
    const QString active_db = QString::fromStdString(q->session_info().database);
    auto token = db_list_cancel_;
    QPointer<MainWindow> self(this);
    QPointer<QueryTabWidget> target_tab(q);

    worker_pool_.submit([self, target_tab, cfg, active_db, token, gen]() {
        auto provider = drivers::create_catalog_provider(cfg.engine);
        auto res = provider->list_databases(cfg, *token);

        QMetaObject::invokeMethod(
            self,
            [self, target_tab, active_db, gen, res = std::move(res)]() {
                if (!self || gen != self->db_list_generation_ || !target_tab || self->active_query() != target_tab) {
                    return;
                }
                if (!res.has_value()) {
                    self->status_label_->setText(
                        QStringLiteral("Could not list databases: %1")
                            .arg(QString::fromStdString(res.error().format_short())));
                    return;
                }

                QStringList list;
                for (const std::string& db : *res) {
                    const QString qdb = QString::fromStdString(db);
                    if (!list.contains(qdb, Qt::CaseInsensitive)) {
                        list.push_back(qdb);
                    }
                }
                if (!active_db.isEmpty() && !list.contains(active_db, Qt::CaseInsensitive)) {
                    list.push_back(active_db);
                }
                std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) {
                    return QString::compare(a, b, Qt::CaseInsensitive) < 0;
                });

                self->db_combo_->clear();
                self->db_combo_->addItems(list);
                const int idx = self->db_combo_->findText(active_db);
                if (idx >= 0) {
                    self->db_combo_->setCurrentIndex(idx);
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::on_database_activated(int index) {
    QueryTabWidget* q = active_query();
    if (q == nullptr || !q->is_connected() || q->is_busy() || index < 0) {
        return;
    }
    const QString chosen = db_combo_->itemText(index);
    if (chosen.isEmpty() || chosen.toStdString() == q->session_info().database) {
        return;
    }
    q->switch_database_async(chosen, &worker_pool_);
}

void MainWindow::run_select_top(
    const drivers::ConnectionConfig& cfg,
    const QString& database,
    const QString& schema,
    const QString& table,
    uint32_t top_n) {
    auto token = std::make_shared<core::CancelToken>();
    QPointer<MainWindow> self(this);
    const drivers::ConnectionConfig target_cfg = database.isEmpty() ? cfg : cfg.with_database(database.toStdString());
    const std::string db_str  = target_cfg.database;
    const std::string sch_str = schema.toStdString();
    const std::string tbl_str = table.toStdString();

    worker_pool_.submit([self, target_cfg, db_str, sch_str, tbl_str, top_n, token]() {
        auto provider = drivers::create_catalog_provider(target_cfg.engine);
        auto cols_res = provider->read_table_columns(target_cfg, db_str, sch_str, tbl_str, *token);
        std::vector<std::string> cols;
        if (cols_res.has_value()) {
            cols = std::move(*cols_res);
        }
        const std::string sql = provider->generate_select_top_sql(sch_str, tbl_str, cols, top_n);

        QMetaObject::invokeMethod(
            self,
            [self, target_cfg, sql = QString::fromStdString(sql)]() {
                if (!self) {
                    return;
                }
                if (QueryTabWidget* q = self->new_query(QString{}, sql)) {
                    q->connect_async(target_cfg, &self->worker_pool_, [q_ptr = QPointer<QueryTabWidget>(q)](bool ok, const QString&) {
                        if (ok && q_ptr) {
                            q_ptr->execute_query();
                        }
                    });
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::run_script_select(
    const drivers::ConnectionConfig& cfg,
    const QString& database,
    const QString& schema,
    const QString& table,
    bool to_new_query) {
    auto token = std::make_shared<core::CancelToken>();
    QPointer<MainWindow> self(this);
    const drivers::ConnectionConfig target_cfg = database.isEmpty() ? cfg : cfg.with_database(database.toStdString());
    const std::string db_str  = target_cfg.database;
    const std::string sch_str = schema.toStdString();
    const std::string tbl_str = table.toStdString();

    worker_pool_.submit([self, target_cfg, db_str, sch_str, tbl_str, to_new_query, token]() {
        auto provider = drivers::create_catalog_provider(target_cfg.engine);
        auto cols_res = provider->read_table_columns(target_cfg, db_str, sch_str, tbl_str, *token);
        std::vector<std::string> cols;
        if (cols_res.has_value()) {
            cols = std::move(*cols_res);
        }
        const std::string sql = provider->generate_select_top_sql(sch_str, tbl_str, cols, 1000u);

        QMetaObject::invokeMethod(
            self,
            [self, target_cfg, sql = QString::fromStdString(sql), to_new_query]() {
                if (!self) {
                    return;
                }
                if (to_new_query) {
                    if (QueryTabWidget* q = self->new_query(QString{}, sql)) {
                        q->connect_async(target_cfg, &self->worker_pool_);
                    }
                } else {
                    if (QClipboard* cb = QGuiApplication::clipboard()) {
                        cb->setText(sql);
                    }
                    self->status_label_->setText(QStringLiteral("SELECT script copied to clipboard."));
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace capybaradb::ui
