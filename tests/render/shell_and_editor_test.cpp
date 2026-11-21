#include "core/config/connection_profile.hpp"
#include "drivers/common/db_driver.hpp"
#include "drivers/common/driver_registry.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"
#include "ui/dialogs/connection_dialog.hpp"
#include <QRadioButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include "ui/dialogs/filter_settings_dialog.hpp"
#include "ui/editor/editor_search_bars.hpp"
#include "ui/editor/query_tab_widget.hpp"
#include "ui/editor/sql_code_editor.hpp"
#include "ui/editor/sql_syntax_highlighter.hpp"
#include "ui/explorer/catalog_tree_model.hpp"
#include "ui/explorer/explorer_context_menu.hpp"
#include "ui/explorer/object_explorer_panel.hpp"
#include "ui/shell/icon_catalog.hpp"
#include "ui/shell/main_window.hpp"
#include "ui/shell/shortcut_router.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>

namespace {

QColor sample_widget_center_color(QWidget* widget) {
    if (widget == nullptr || widget->width() <= 4 || widget->height() <= 4) {
        return {};
    }
    const QPixmap pm = widget->grab();
    if (pm.isNull()) {
        return {};
    }
    const QImage img = pm.toImage();
    return img.pixelColor(img.width() / 2, img.height() / 2);
}

QColor sample_widget_color_at(QWidget* widget, int logical_x, int logical_y) {
    if (widget == nullptr) {
        return {};
    }
    const QPixmap pm = widget->grab();
    if (pm.isNull()) {
        return {};
    }
    const QImage img = pm.toImage();
    const qreal dpr = pm.devicePixelRatio();
    const int px = std::clamp(static_cast<int>(logical_x * dpr), 0, img.width() - 1);
    const int py = std::clamp(static_cast<int>(logical_y * dpr), 0, img.height() - 1);
    return img.pixelColor(px, py);
}

class MockCatalogProvider final : public capybaradb::drivers::CatalogProvider {
public:
    std::expected<std::vector<capybaradb::drivers::CatalogNodeData>, capybaradb::drivers::DbError> list_children(
        const capybaradb::drivers::ConnectionConfig&,
        const capybaradb::drivers::CatalogNodeData& parent,
        capybaradb::core::CancelToken&) override {
        using capybaradb::drivers::CatalogKind;
        using capybaradb::drivers::CatalogNodeData;

        auto folder = [&](CatalogKind kind, std::string name) {
            return CatalogNodeData::make_folder(kind, std::move(name), parent.database, parent.schema, parent.oid);
        };

        switch (parent.kind) {
        case CatalogKind::Server:
            return std::vector<CatalogNodeData>{
                folder(CatalogKind::Databases, "Databases"),
                folder(CatalogKind::Roles, "Login/Group Roles"),
            };
        case CatalogKind::Databases: {
            CatalogNodeData db;
            db.kind = CatalogKind::Database;
            db.name = "appdb";
            db.database = "appdb";
            db.oid = 100;
            return std::vector<CatalogNodeData>{db};
        }
        case CatalogKind::Database:
            return std::vector<CatalogNodeData>{
                folder(CatalogKind::Schemas, "Schemas"),
            };
        case CatalogKind::Schemas: {
            CatalogNodeData sch;
            sch.kind = CatalogKind::Schema;
            sch.name = "public";
            sch.database = parent.database;
            sch.schema = "public";
            sch.oid = 2200;
            return std::vector<CatalogNodeData>{sch};
        }
        case CatalogKind::Schema:
            return std::vector<CatalogNodeData>{
                folder(CatalogKind::Tables, "Tables"),
                folder(CatalogKind::Views, "Views"),
            };
        case CatalogKind::Tables: {
            CatalogNodeData t1;
            t1.kind = CatalogKind::Relation;
            t1.name = "orders";
            t1.database = parent.database;
            t1.schema = parent.schema;
            t1.oid = 1001;
            t1.relation_type = 'r';
            t1.owner = "alice";

            CatalogNodeData t2;
            t2.kind = CatalogKind::Relation;
            t2.name = "users";
            t2.database = parent.database;
            t2.schema = parent.schema;
            t2.oid = 1002;
            t2.relation_type = 'r';
            t2.owner = "bob";
            return std::vector<CatalogNodeData>{t1, t2};
        }
        default:
            return std::vector<CatalogNodeData>{};
        }
    }

    std::expected<std::vector<std::string>, capybaradb::drivers::DbError> list_databases(
        const capybaradb::drivers::ConnectionConfig&,
        capybaradb::core::CancelToken&) override {
        return std::vector<std::string>{"appdb", "postgres"};
    }

    std::expected<std::vector<std::string>, capybaradb::drivers::DbError> read_table_columns(
        const capybaradb::drivers::ConnectionConfig&,
        std::string_view,
        std::string_view,
        std::string_view,
        capybaradb::core::CancelToken&) override {
        return std::vector<std::string>{"id", "created_at"};
    }

    std::string generate_select_top_sql(
        std::string_view schema,
        std::string_view object,
        std::span<const std::string>,
        uint32_t top_n) const override {
        return "SELECT * FROM \"" + std::string(schema) + "\".\"" + std::string(object) +
               "\" LIMIT " + std::to_string(top_n) + ";";
    }
};

} // namespace

class ShellAndEditorTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        QVERIFY(app != nullptr);

        // Monitor Qt warnings during stylesheet parse & theme application
        static QStringList qss_warnings;
        qss_warnings.clear();
        auto prev_handler = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& msg) {
            if (type == QtWarningMsg || type == QtCriticalMsg) {
                if (msg.contains(QStringLiteral("Could not parse stylesheet"), Qt::CaseInsensitive)) {
                    qss_warnings.append(msg);
                }
            }
        });

        QVERIFY(capybaradb::ui::theme::apply_classic_blue_theme(*app));
        qInstallMessageHandler(prev_handler);
        QVERIFY2(qss_warnings.isEmpty(), qPrintable(qss_warnings.join(u'\n')));
    }

    void classic_blue_theme_applies_expected_color_tokens_and_metrics() {
        using namespace capybaradb::ui::theme;
        QCOMPARE(color(kShellBg), QColor(0xA8, 0xC7, 0xD9));
        QCOMPARE(color(kPanelBg), QColor(0xF5, 0xFA, 0xFE));
        QCOMPARE(color(kToolbarBg), QColor(0xD5, 0xE5, 0xEF));
        QCOMPARE(color(kSplitterBg), QColor(0xD6, 0xE3, 0xED));
        QCOMPARE(color(kMainStatusBarBg), QColor(0x42, 0x5C, 0x6C));
        QCOMPARE(color(kOutputTabStripBg), QColor(0xEE, 0xEE, 0xEE));
        QCOMPARE(color(kQueryStatusNormalBg), QColor(0xFF, 0xF4, 0x9C));
        QCOMPARE(color(kQueryStatusErrorBg), QColor(0xFD, 0xE7, 0xE9));
        QCOMPARE(kQueryStatusBarHeightPx, 24);
        QCOMPARE(kOutputTabStripHeightPx, 25);
        QCOMPARE(kObjectExplorerInitialWidthPx, 300);

        const QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        QVERIFY(app != nullptr);
        QCOMPARE(app->palette().color(QPalette::Window), color(kShellBg));
        QCOMPARE(app->palette().color(QPalette::Highlight), color(kTreeSelectionActiveBg));
    }

    void icon_catalog_loads_all_svg_assets_at_standard_and_hidpi_scales() {
        const auto ids = capybaradb::ui::all_icon_ids();
        QVERIFY(ids.size() >= 50);

        for (const capybaradb::ui::IconId id : ids) {
            const QIcon& icon = capybaradb::ui::get_icon(id);
            QVERIFY2(!icon.isNull(), capybaradb::ui::icon_resource_path(id));

            const QPixmap pm1x = capybaradb::ui::render_icon_pixmap(id, 16, 1.0);
            QVERIFY(!pm1x.isNull());
            QCOMPARE(pm1x.size(), QSize(16, 16));

            const QPixmap pm2x = capybaradb::ui::render_icon_pixmap(id, 16, 2.0);
            QVERIFY(!pm2x.isNull());
            QCOMPARE(pm2x.size(), QSize(32, 32));
            QCOMPARE(pm2x.devicePixelRatio(), 2.0);
        }
    }

    void main_window_manages_query_tabs_shortcut_chords_and_layout_persistence() {
        QTemporaryDir state_dir;
        QVERIFY(state_dir.isValid());

        capybaradb::ui::MainWindow win(std::filesystem::path(state_dir.path().toStdWString()));
        win.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) {
            return QMessageBox::No;
        });
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QTest::qWait(20);

        // Initial disconnected state checks (M1.5)
        QCOMPARE(win.windowTitle(), QStringLiteral("CapybaraDB"));
        QCOMPARE(win.query_tab_count(), 0);
        QVERIFY(!win.action_execute()->isEnabled());
        QVERIFY(!win.action_cancel()->isEnabled());
        QVERIFY(!win.available_databases_combo()->isEnabled());
        QCOMPARE(win.available_databases_combo()->currentText(), QStringLiteral("No active query"));
        QVERIFY(!win.action_results_text()->isEnabled());
        QVERIFY(!win.action_results_grid()->isEnabled());
        QVERIFY(!win.action_results_file()->isEnabled());
        QCOMPARE(win.available_databases_combo()->width(), capybaradb::ui::theme::kAvailableDbComboWidthPx);

        // Object Explorer must stay ~300 px wide with empty #A8C7D9 workspace filling the right side
        QVERIFY(win.explorer_dock()->dockAreaWidget() != nullptr);
        QVERIFY(win.explorer_dock()->dockAreaWidget()->width() <= 360);
        QVERIFY(win.explorer_dock()->dockAreaWidget()->width() < win.width() / 2);
        QCOMPARE(
            sample_widget_color_at(win.dock_manager(), win.dock_manager()->width() - 40, win.dock_manager()->height() / 2),
            QColor(0xA8, 0xC7, 0xD9));

        // Open SQLQuery1.sql & SQLQuery2.sql via Ctrl+N
        QTest::keyClick(&win, Qt::Key_N, Qt::ControlModifier);
        QTest::qWait(20);
        QCOMPARE(win.query_tab_count(), 1);
        QCOMPARE(win.active_tab_title(), QStringLiteral("SQLQuery1.sql"));
        QCOMPARE(win.available_databases_combo()->currentText(), QStringLiteral("Not connected"));
        QVERIFY(win.explorer_dock()->dockAreaWidget()->width() < win.width() / 2);

        QTest::keyClick(&win, Qt::Key_N, Qt::ControlModifier);
        QTest::qWait(20);
        QCOMPARE(win.query_tab_count(), 2);
        QCOMPARE(win.active_tab_title(), QStringLiteral("SQLQuery2.sql"));

        capybaradb::ui::QueryTabWidget* q2 = win.active_query();
        QVERIFY(q2 != nullptr);

        // Results destination actions enabled when tab active; Execute still disabled (unconnected)
        QVERIFY(!win.action_execute()->isEnabled());
        QVERIFY(win.action_results_grid()->isEnabled());
        QVERIFY(win.action_results_grid()->isChecked());

        // Structural dimensions and exact parity colors (M1.8)
        QCOMPARE(q2->query_status_bar()->height(), 24);
        QCOMPARE(q2->output_strip_bar()->height(), 25);

        QCOMPARE(sample_widget_color_at(&win, 10, 10), QColor(0xA8, 0xC7, 0xD9)); // ShellBg
        QCOMPARE(sample_widget_color_at(win.toolbar_band0(), 4, 4), QColor(0xD5, 0xE5, 0xEF)); // ToolbarBg
        QCOMPARE(sample_widget_color_at(win.global_status_bar(), 4, 4), QColor(0x42, 0x5C, 0x6C)); // MainStatusBarBg
        QCOMPARE(sample_widget_color_at(win.object_explorer(), 4, 4), QColor(0xF5, 0xFA, 0xFE)); // PanelBg
        QCOMPARE(sample_widget_color_at(q2->output_strip_bar(), 1, 10), QColor(0xEE, 0xEE, 0xEE)); // OutputTabStripBg
        QCOMPARE(sample_widget_color_at(q2->query_status_bar(), 2, 12), QColor(0xFF, 0xF4, 0x9C)); // QueryStatusNormalBg

        q2->set_status_state(capybaradb::ui::QueryTabWidget::StatusKind::Error, QStringLiteral("Error state"), true);
        QCOMPARE(sample_widget_color_at(q2->query_status_bar(), 2, 12), QColor(0xFD, 0xE7, 0xE9)); // QueryStatusErrorBg
        q2->set_status_state(capybaradb::ui::QueryTabWidget::StatusKind::Idle, QStringLiteral("Não conectado"), false);

        // Ctrl+R toggles bottom results pane visibility
        QVERIFY(q2->is_results_pane_visible());
        QTest::keyClick(&win, Qt::Key_R, Qt::ControlModifier);
        QTest::qWait(20);
        QVERIFY(!q2->is_results_pane_visible());
        QTest::keyClick(&win, Qt::Key_R, Qt::ControlModifier);
        QTest::qWait(20);
        QVERIFY(q2->is_results_pane_visible());

        // Hide Object Explorer, then press F8 to bring it back and focus it
        win.explorer_dock()->toggleView(false);
        QTest::qWait(20);
        QVERIFY(win.explorer_dock()->isClosed());
        QTest::keyClick(&win, Qt::Key_F8, Qt::NoModifier);
        QTest::qWait(20);
        QVERIFY(!win.explorer_dock()->isClosed());

        // ShortcutRouter chords & contextual signals
        auto* router = win.shortcut_router();
        QVERIFY(router != nullptr);
        QSignalSpy comment_spy(router, &capybaradb::ui::ShortcutRouter::comment_selection_requested);
        QSignalSpy uncomment_spy(router, &capybaradb::ui::ShortcutRouter::uncomment_selection_requested);
        QSignalSpy intellisense_spy(router, &capybaradb::ui::ShortcutRouter::toggle_intellisense_requested);
        QSignalSpy cancel_spy(router, &capybaradb::ui::ShortcutRouter::cancel_requested);
        QSignalSpy refresh_spy(router, &capybaradb::ui::ShortcutRouter::refresh_explorer_requested);
        QSignalSpy exec_spy(router, &capybaradb::ui::ShortcutRouter::execute_requested);

        // Ctrl+K, Ctrl+C
        QKeyEvent k_press(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier);
        QKeyEvent c_press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(router->process_key_event(&k_press, false));
        QCOMPARE(router->active_chord(), capybaradb::ui::ShortcutRouter::ChordKind::CtrlK);
        QVERIFY(router->process_key_event(&c_press, false));
        QCOMPARE(comment_spy.count(), 1);

        // Ctrl+K, Esc cancels chord
        QKeyEvent esc_press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QVERIFY(router->process_key_event(&k_press, false));
        QVERIFY(router->process_key_event(&esc_press, false));
        QCOMPARE(router->active_chord(), capybaradb::ui::ShortcutRouter::ChordKind::None);

        // Ctrl+B, Ctrl+I toggles IntelliSense
        QKeyEvent b_press(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier);
        QKeyEvent i_press(QEvent::KeyPress, Qt::Key_I, Qt::ControlModifier);
        QVERIFY(router->process_key_event(&b_press, false));
        QVERIFY(router->process_key_event(&i_press, false));
        QCOMPARE(intellisense_spy.count(), 1);
        QCOMPARE(uncomment_spy.count(), 0);

        // Alt+Break triggers cancel
        QKeyEvent alt_break(QEvent::KeyPress, Qt::Key_Pause, Qt::AltModifier);
        QVERIFY(router->process_key_event(&alt_break, false));
        QCOMPARE(cancel_spy.count(), 1);

        // Contextual F5: Explorer focused -> refresh_explorer; Editor focused -> execute
        win.object_explorer()->focus_tree();
        QTest::qWait(10);
        QKeyEvent f5_press(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
        QVERIFY(router->process_key_event(&f5_press, false));
        QCOMPARE(refresh_spy.count(), 1);

        q2->focus_editor();
        QTest::qWait(10);
        QVERIFY(router->process_key_event(&f5_press, false));
        QCOMPARE(exec_spy.count(), 1);

        // Float a tab and test Reset Window Layout preserves all open tabs
        ads::CDockWidget* d2 = win.query_docks().back();
        win.dock_manager()->addDockWidgetFloating(d2);
        QTest::qWait(20);
        QVERIFY(d2->isInFloatingContainer());

        win.reset_window_layout();
        QTest::qWait(20);
        QCOMPARE(win.query_tab_count(), 2);
        QVERIFY(!d2->isInFloatingContainer());

        // Ctrl+F4 closes active tab
        QTest::keyClick(&win, Qt::Key_F4, Qt::ControlModifier);
        QTest::qWait(20);
        QCOMPARE(win.query_tab_count(), 1);
        QCOMPARE(win.active_tab_title(), QStringLiteral("SQLQuery1.sql"));

        // Connected query state updates 6 status bar segments & AvailableDatabasesCombo (M2.7)
        capybaradb::drivers::ConnectionConfig pg_cfg;
        pg_cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        pg_cfg.host     = "localhost";
        pg_cfg.port     = 5432;
        pg_cfg.database = "appdb";
        pg_cfg.username = "postgres";

        capybaradb::drivers::SessionInfo s_info;
        s_info.engine         = capybaradb::drivers::EngineKind::PostgreSql;
        s_info.host           = "localhost";
        s_info.port           = 5432;
        s_info.server_version = "PostgreSQL 17.2 on x86_64";
        s_info.database       = "appdb";
        s_info.username       = "postgres";
        s_info.session_id     = 4242;

        capybaradb::ui::QueryTabWidget* q1 = win.active_query();
        QVERIFY(q1 != nullptr);
        q1->set_connected_state(pg_cfg, s_info);
        QTest::qWait(10);
        QVERIFY(q1->status_server_label()->text().contains(QStringLiteral("PostgreSQL 17.2")));
        QCOMPARE(q1->status_user_label()->text(), QStringLiteral("postgres (PID: 4242)"));
        QCOMPARE(q1->status_database_label()->text(), QStringLiteral("appdb"));
        QVERIFY(win.action_execute()->isEnabled());
        QVERIFY(win.available_databases_combo()->isEnabled());
        QCOMPARE(win.available_databases_combo()->currentText(), QStringLiteral("appdb"));

        q1->disconnect_session();
        QTest::qWait(10);
        QVERIFY(!win.action_execute()->isEnabled());
        QVERIFY(!win.available_databases_combo()->isEnabled());
        QCOMPARE(win.available_databases_combo()->currentText(), QStringLiteral("Not connected"));

        // Persist geometry & layout, then verify restore + corrupt fallback
        win.resize(1100, 720);
        win.save_persistent_state();
        win.close();

        capybaradb::ui::MainWindow win2(std::filesystem::path(state_dir.path().toStdWString()));
        QCOMPARE(win2.size(), QSize(1100, 720));

        // Corrupt layout.bin and ensure safe fallback without crash
        const std::filesystem::path layout_file =
            std::filesystem::path(state_dir.path().toStdWString()) / "layout.bin";
        {
            std::ofstream corrupt(layout_file, std::ios::binary | std::ios::trunc);
            corrupt << "NOT_A_VALID_ADS_XML_LAYOUT";
        }
        capybaradb::ui::MainWindow win3(std::filesystem::path(state_dir.path().toStdWString()));
        QVERIFY(win3.explorer_dock() != nullptr);
        QVERIFY(win3.explorer_dock()->dockAreaWidget() != nullptr);
        QVERIFY(win3.global_status_label()->text().contains(QStringLiteral("default layout")));
    }

    void connection_dialog_switches_engine_pages_manages_profiles_and_parses_uris() {
        QTemporaryDir state_dir;
        QVERIFY(state_dir.isValid());
        const auto state_path = std::filesystem::path(state_dir.path().toStdWString());

        // Ensure zero driver plugins are loaded before opening ConnectionDialog
        (void)capybaradb::drivers::unload_unused_driver_modules(std::chrono::milliseconds{0});
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::PostgreSql));
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::SqlServer));
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::Sqlite));

        capybaradb::ui::ConnectionDialog dlg(nullptr, state_path);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QCOMPARE(dlg.size(), QSize(720, 520));
        QCOMPARE(dlg.minimumSize(), QSize(620, 440));
        QCOMPARE(dlg.pages_stack()->count(), 2); // Strictly 2 reusable sub-forms (ServerConnectionPage + FileConnectionPage)

        // Default engine is PostgreSQL (ServerConnectionPage active)
        QCOMPARE(dlg.current_engine(), capybaradb::drivers::EngineKind::PostgreSql);
        QCOMPARE(dlg.pages_stack()->currentWidget(), dlg.server_page());
        QCOMPARE(dlg.host_edit()->text(), QStringLiteral("localhost"));
        QCOMPARE(dlg.port_edit()->text(), QStringLiteral("5432"));
        QCOMPARE(dlg.database_edit()->text(), QStringLiteral("postgres"));
        QCOMPARE(dlg.username_edit()->text(), QStringLiteral("postgres"));
        QVERIFY(!dlg.auth_combo()->isVisible()); // Authentication row hidden in PostgreSQL (!supports_integrated_auth)
        QVERIFY(dlg.username_edit()->isVisible());
        QVERIFY(dlg.password_edit()->isVisible());

        // Verify exact dialog & banner background colors from PARITY_SPEC.md
        QCOMPARE(sample_widget_color_at(&dlg, 10, 10), QColor(0xE4, 0xEF, 0xF6)); // DialogHeaderBg
        QCOMPARE(sample_widget_color_at(&dlg, dlg.width() - 20, dlg.height() - 10), QColor(0xF5, 0xF5, 0xF5)); // DialogBg

        // Toggle Server type PostgreSQL -> SQL Server
        dlg.engine_button(capybaradb::drivers::EngineKind::SqlServer)->click();
        QCOMPARE(dlg.current_engine(), capybaradb::drivers::EngineKind::SqlServer);
        QCOMPARE(dlg.pages_stack()->currentWidget(), dlg.server_page());
        QCOMPARE(dlg.port_edit()->text(), QStringLiteral("1433"));
        QCOMPARE(dlg.database_edit()->text(), QStringLiteral("master"));
        QCOMPARE(dlg.username_edit()->text(), QStringLiteral("sa"));
        QVERIFY(dlg.auth_combo()->isVisible()); // Authentication row visible in SQL Server
        QVERIFY(dlg.auth_combo()->isEnabled());

        // Switch to Security / TLS tab and verify root_cert_edit is hidden in SQL Server (!supports_root_cert)
        dlg.server_page()->tab_widget()->setCurrentIndex(1);
        QVERIFY(!dlg.root_cert_edit()->isVisible());
        dlg.server_page()->tab_widget()->setCurrentIndex(0);

        dlg.auth_combo()->setCurrentIndex(1); // Windows / Kerberos
        QVERIFY(!dlg.username_edit()->isVisible()); // Real row hiding via setRowVisible
        QVERIFY(!dlg.password_edit()->isVisible());
        QVERIFY(!dlg.remember_password_check()->isVisible());

        // Toggle to SQLite (FileConnectionPage active -> zero network/credential fields visible)
        dlg.engine_button(capybaradb::drivers::EngineKind::Sqlite)->click();
        QCOMPARE(dlg.current_engine(), capybaradb::drivers::EngineKind::Sqlite);
        QCOMPARE(dlg.pages_stack()->currentWidget(), dlg.file_page());
        QVERIFY(!dlg.host_edit()->isVisible());
        QVERIFY(!dlg.port_edit()->isVisible());
        QVERIFY(!dlg.auth_combo()->isVisible());
        QVERIFY(!dlg.username_edit()->isVisible());
        QVERIFY(!dlg.password_edit()->isVisible());
        QVERIFY(!dlg.ssl_combo()->isVisible());
        QVERIFY(!dlg.root_cert_edit()->isVisible());
        QVERIFY(dlg.file_page()->file_path_edit()->isVisible());
        QVERIFY(dlg.file_page()->browse_button()->isVisible());

        // In-memory mode hides file path row
        dlg.file_page()->in_memory_radio()->click();
        QVERIFY(!dlg.file_page()->file_path_edit()->isVisible());
        dlg.file_page()->existing_file_radio()->click();
        QVERIFY(dlg.file_page()->file_path_edit()->isVisible());

        // Zero driver DLLs loaded after opening and navigating across all 3 engines
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::PostgreSql));
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::SqlServer));
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::Sqlite));

        // Switch back to PostgreSQL
        dlg.engine_button(capybaradb::drivers::EngineKind::PostgreSql)->click();
        QCOMPARE(dlg.port_edit()->text(), QStringLiteral("5432"));
        QCOMPARE(dlg.database_edit()->text(), QStringLiteral("postgres"));
        QVERIFY(dlg.username_edit()->isVisible());

        // Save PostgreSQL profile with group & password, reset form, reselect profile and verify password reveal
        dlg.profile_name_edit()->setText(QStringLiteral("SyntheticPG"));
        dlg.profile_group_combo()->setEditText(QStringLiteral("Production"));
        dlg.host_edit()->setText(QStringLiteral("127.0.0.1"));
        dlg.database_edit()->setText(QStringLiteral("capybaradb_synthetic"));
        dlg.username_edit()->setText(QStringLiteral("capybaradb_user"));
        dlg.password_edit()->setText(QStringLiteral("secret123"));
        dlg.remember_password_check()->setChecked(true);
        dlg.save_profile_button()->click();
        QVERIFY(dlg.status_label()->text().contains(QStringLiteral("saved"), Qt::CaseInsensitive));
        QCOMPARE(dlg.profiles_combo()->count(), 2);
        QCOMPARE(dlg.sidebar_widget()->visible_profile_count(), 1);

        // Toggle password eye icon inline action
        QCOMPARE(dlg.password_edit()->echoMode(), QLineEdit::Password);
        dlg.server_page()->toggle_password_action()->trigger();
        QCOMPARE(dlg.password_edit()->echoMode(), QLineEdit::Normal);
        dlg.server_page()->toggle_password_action()->trigger();
        QCOMPARE(dlg.password_edit()->echoMode(), QLineEdit::Password);

        // Click "+ New Connection" in sidebar -> clears password, then reselect profile
        dlg.sidebar_widget()->new_button()->click();
        QVERIFY(dlg.password_edit()->text().isEmpty());
        dlg.profiles_combo()->setCurrentIndex(1);
        QCOMPARE(dlg.host_edit()->text(), QStringLiteral("127.0.0.1"));
        QCOMPARE(dlg.database_edit()->text(), QStringLiteral("capybaradb_synthetic"));
        QCOMPARE(dlg.password_edit()->text(), QStringLiteral("secret123"));

        // Duplicate profile via sidebar button and save as SQLite profile in "Local" group
        dlg.duplicate_profile_button()->click();
        QVERIFY(dlg.selected_profile_id().isEmpty());
        QCOMPARE(dlg.profile_name_edit()->text(), QStringLiteral("SyntheticPG (Copy)"));

        const QString sqlite_db_file = state_dir.path() + QStringLiteral("/local_cache.db");
        {
            QFile f(sqlite_db_file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.close();
        }
        dlg.engine_button(capybaradb::drivers::EngineKind::Sqlite)->click();
        dlg.profile_name_edit()->setText(QStringLiteral("App Cache.db"));
        dlg.profile_group_combo()->setEditText(QStringLiteral("Local"));
        dlg.file_page()->set_file_picker([sqlite_db_file](bool, const QString&) { return sqlite_db_file; });
        dlg.file_page()->browse_button()->click();
        QVERIFY(!dlg.file_page()->file_path_edit()->text().isEmpty());
        dlg.save_profile_button()->click();
        QCOMPARE(dlg.profiles_combo()->count(), 3);
        QCOMPARE(dlg.sidebar_widget()->visible_profile_count(), 2);

        // Instant sidebar filter
        dlg.sidebar_widget()->filter_edit()->setText(QStringLiteral("Cache"));
        QCOMPARE(dlg.sidebar_widget()->visible_profile_count(), 1);
        dlg.sidebar_widget()->filter_edit()->setText(QStringLiteral("Production"));
        QCOMPARE(dlg.sidebar_widget()->visible_profile_count(), 1);
        dlg.sidebar_widget()->filter_edit()->clear();
        QCOMPARE(dlg.sidebar_widget()->visible_profile_count(), 2);

        // Paste / apply connection URI
        QVERIFY(dlg.apply_connection_uri(QStringLiteral("postgresql://uri_user:uri_pw@10.20.30.40:5439/uri_db?sslmode=prefer")));
        QCOMPARE(dlg.current_engine(), capybaradb::drivers::EngineKind::PostgreSql);
        QCOMPARE(dlg.host_edit()->text(), QStringLiteral("10.20.30.40"));
        QCOMPARE(dlg.port_edit()->text(), QStringLiteral("5439"));
        QCOMPARE(dlg.database_edit()->text(), QStringLiteral("uri_db"));
        QCOMPARE(dlg.username_edit()->text(), QStringLiteral("uri_user"));
        QCOMPARE(dlg.password_edit()->text(), QStringLiteral("uri_pw"));

        // Reselect SyntheticPG, accept and verify ConnectionConfig & display_name
        dlg.profiles_combo()->setCurrentIndex(2); // SyntheticPG (sorted after App Cache.db)
        dlg.connect_button()->click();
        QCOMPARE(dlg.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dlg.connection_config().host, std::string{"127.0.0.1"});
        QCOMPARE(dlg.connection_config().database, std::string{"capybaradb_synthetic"});
        QCOMPARE(dlg.connection_config().password, std::string{"secret123"});
        QCOMPARE(dlg.display_name(), QStringLiteral("127.0.0.1:5432 (capybaradb_user)"));

        // Verify SQLite :memory: and file display_name (no "localhost:0") and delete profile
        capybaradb::ui::ConnectionDialog dlg2(nullptr, state_path);
        dlg2.set_delete_confirmation_handler([]() { return true; });
        QCOMPARE(dlg2.profiles_combo()->count(), 3);
        dlg2.profiles_combo()->setCurrentIndex(1); // App Cache.db
        QCOMPARE(dlg2.current_engine(), capybaradb::drivers::EngineKind::Sqlite);
        dlg2.delete_profile_button()->click();
        QCOMPARE(dlg2.profiles_combo()->count(), 2);

        dlg2.engine_button(capybaradb::drivers::EngineKind::Sqlite)->click();
        dlg2.file_page()->in_memory_radio()->click();

        // Run asynchronous connection test against SQLite :memory: and verify driver module unloads right after test
        dlg2.start_test_connection();
        QTRY_VERIFY(dlg2.status_label()->text().contains(QStringLiteral("succeeded"), Qt::CaseInsensitive));
        QVERIFY(!capybaradb::drivers::is_driver_module_loaded(capybaradb::drivers::EngineKind::Sqlite));

        dlg2.connect_button()->click();
        QCOMPARE(dlg2.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dlg2.connection_config().engine, capybaradb::drivers::EngineKind::Sqlite);
        QCOMPARE(dlg2.connection_config().port, static_cast<uint16_t>(0));
        QCOMPARE(dlg2.connection_config().database, std::string{":memory:"});
        QCOMPARE(dlg2.display_name(), QStringLiteral(":memory:"));
    }

    void object_explorer_lazy_loads_catalog_nodes_applies_filters_and_builds_context_menus() {
        QTemporaryDir state_dir;
        QVERIFY(state_dir.isValid());
        const auto prefs_path = std::filesystem::path(state_dir.path().toStdWString()) / "explorer-toolbar.json";

        // FilterSettingsDialog parity & result check
        capybaradb::drivers::CatalogFilter initial_filter;
        capybaradb::ui::FilterSettingsDialog f_dlg(
            QStringLiteral("localhost:5432"),
            QStringLiteral("appdb"),
            initial_filter,
            true);
        f_dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&f_dlg));
        QCOMPARE(f_dlg.criteria_table()->rowCount(), 2);

        f_dlg.name_value_edit()->setText(QStringLiteral("orders"));
        f_dlg.owner_value_edit()->setText(QStringLiteral("alice"));
        f_dlg.ok_button()->click();
        QCOMPARE(f_dlg.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(f_dlg.filter_result().name, std::string{"orders"});
        QCOMPARE(f_dlg.filter_result().owner, std::string{"alice"});

        // ObjectExplorerPanel lazy loading, filtering & context menu
        capybaradb::core::CatalogWorkerPool pool(2);
        capybaradb::ui::ObjectExplorerPanel panel(prefs_path, &pool);
        panel.catalog_model()->set_factories(
            {},
            [](capybaradb::drivers::EngineKind) {
                return std::make_unique<MockCatalogProvider>();
            });

        capybaradb::drivers::ConnectionConfig cfg;
        cfg.host     = "localhost";
        cfg.port     = 5432;
        cfg.database = "postgres";
        cfg.username = "postgres";

        capybaradb::drivers::SessionInfo info;
        info.engine         = capybaradb::drivers::EngineKind::PostgreSql;
        info.host           = "localhost";
        info.port           = 5432;
        info.server_version = "PostgreSQL 17.2";
        info.database       = "postgres";
        info.username       = "postgres";
        info.session_id     = 101;

        panel.set_connected_server(cfg, info);
        QVERIFY(panel.is_connected());
        QVERIFY(panel.toolbar_button("Disconnect")->isEnabled());

        auto* model = panel.catalog_model();
        const QModelIndex root_idx = model->index(0, 0);
        QTRY_COMPARE(model->rowCount(root_idx), 2); // Databases + Login/Group Roles
        QTRY_COMPARE(model->data(model->index(0, 0, root_idx)).toString(), QStringLiteral("Databases"));

        const QModelIndex dbs_idx = model->index(0, 0, root_idx);
        model->load_node_async(dbs_idx, false);
        QTRY_COMPARE(model->data(model->index(0, 0, dbs_idx)).toString(), QStringLiteral("appdb"));

        const QModelIndex appdb_idx = model->index(0, 0, dbs_idx);
        model->load_node_async(appdb_idx, false);
        QTRY_COMPARE(model->data(model->index(0, 0, appdb_idx)).toString(), QStringLiteral("Schemas"));

        const QModelIndex schemas_idx = model->index(0, 0, appdb_idx);
        model->load_node_async(schemas_idx, false);
        QTRY_COMPARE(model->data(model->index(0, 0, schemas_idx)).toString(), QStringLiteral("public"));

        const QModelIndex public_idx = model->index(0, 0, schemas_idx);
        model->load_node_async(public_idx, false);
        QTRY_COMPARE(model->rowCount(public_idx), 2); // Tables + Views
        QTRY_COMPARE(model->data(model->index(0, 0, public_idx)).toString(), QStringLiteral("Tables"));

        const QModelIndex tables_idx = model->index(0, 0, public_idx);
        model->load_node_async(tables_idx, false);
        QTRY_COMPARE(model->rowCount(tables_idx), 2); // orders + users
        QTRY_COMPARE(model->data(model->index(0, 0, tables_idx)).toString(), QStringLiteral("orders"));

        // Apply Name + Owner filter on Tables folder
        capybaradb::drivers::CatalogFilter tbl_filter;
        tbl_filter.name = "ord";
        tbl_filter.name_operator = capybaradb::drivers::CatalogFilterOperator::Contains;
        tbl_filter.owner = "alice";
        tbl_filter.owner_operator = capybaradb::drivers::CatalogFilterOperator::Equals;
        model->apply_filter(tables_idx, tbl_filter);
        QCOMPARE(model->rowCount(tables_idx), 1);
        QCOMPARE(model->data(model->index(0, 0, tables_idx)).toString(), QStringLiteral("orders"));
        QVERIFY(model->data(tables_idx).toString().contains(QStringLiteral("(filtered)")));

        // Context menu inspection on a table relation node
        const QModelIndex orders_idx = model->index(0, 0, tables_idx);
        const capybaradb::ui::CatalogTreeNode* orders_node = model->node_from_index(orders_idx);
        QVERIFY(orders_node != nullptr);
        QMenu* ctx = capybaradb::ui::build_explorer_context_menu(orders_node, orders_idx, 1000, {}, &panel);
        QVERIFY(ctx != nullptr);
        QStringList labels;
        for (QAction* act : ctx->actions()) {
            if (!act->isSeparator()) {
                labels.push_back(act->text());
            }
        }
        QVERIFY(labels.contains(QStringLiteral("Select Top 1000 Rows")));
        QVERIFY(labels.contains(QStringLiteral("Script &Table as")));
        ctx->deleteLater();

        panel.disconnect_all();
        QVERIFY(!panel.is_connected());
        QVERIFY(!panel.toolbar_button("Disconnect")->isEnabled());
    }

    void sql_code_editor_highlights_dialects_toggles_comments_and_executes_find_replace() {
        capybaradb::ui::QueryTabWidget tab(capybaradb::core::ResultsDestination::Text);
        tab.resize(800, 600);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));
        capybaradb::ui::SqlCodeEditor* ed = tab.editor_widget();
        QVERIFY(ed != nullptr);
        QVERIFY(ed->syntax_highlighter() != nullptr);
        QVERIFY(ed->line_number_area_widget() != nullptr);
        QVERIFY(ed->line_number_area_width() >= 20);

        // Verify PostgreSQL lexical token colors on a single representative block
        ed->set_engine_dialect(capybaradb::drivers::EngineKind::PostgreSql);
        tab.set_editor_text(QStringLiteral("SELECT /* c */ 's' \"id\" $tag$d$tag$ -- tail"));
        ed->syntax_highlighter()->rehighlight();
        const QTextBlock pg_block = ed->document()->firstBlock();
        const auto pg_ranges = pg_block.layout()->formats();
        auto has_color = [](const QList<QTextLayout::FormatRange>& ranges, const char* hex) {
            const QColor expected(QString::fromLatin1(hex));
            for (const auto& r : ranges) {
                if (r.format.foreground().color() == expected) {
                    return true;
                }
            }
            return false;
        };
        auto count_color = [](const QList<QTextLayout::FormatRange>& ranges, const char* hex) {
            const QColor expected(QString::fromLatin1(hex));
            int hits = 0;
            for (const auto& r : ranges) {
                if (r.format.foreground().color() == expected) {
                    ++hits;
                }
            }
            return hits;
        };
        QVERIFY(has_color(pg_ranges, "#0000FF")); // Keyword
        QVERIFY(has_color(pg_ranges, "#008000")); // Comment
        QVERIFY(count_color(pg_ranges, "#A31515") >= 3); // String + QuotedIdentifier + DollarString

        // Switch dialect to T-SQL and check local/global variable and [bracket] colors
        ed->set_engine_dialect(capybaradb::drivers::EngineKind::SqlServer);
        tab.set_editor_text(QStringLiteral("SELECT @v, @@SPID, [My Col]"));
        ed->syntax_highlighter()->rehighlight();
        const auto ts_ranges = ed->document()->firstBlock().layout()->formats();
        QVERIFY(has_color(ts_ranges, "#0000FF")); // Keyword
        QVERIFY(has_color(ts_ranges, "#008080")); // @local variable
        QVERIFY(has_color(ts_ranges, "#FF00FF")); // @@global variable
        QVERIFY(has_color(ts_ranges, "#1E1E1E")); // [Bracket identifier]

        // Comment / Uncomment on multi-line selection (single Undo step)
        tab.set_editor_text(QStringLiteral("SELECT 1;\nSELECT 2;"));
        QTextCursor sel_all = ed->textCursor();
        sel_all.select(QTextCursor::Document);
        ed->setTextCursor(sel_all);
        const auto c_res = ed->comment_selection(false);
        QVERIFY(c_res.has_value());
        QCOMPARE(*c_res, 2);
        QCOMPARE(ed->toPlainText(), QStringLiteral("--SELECT 1;\n--SELECT 2;"));
        ed->undo();
        QCOMPARE(ed->toPlainText(), QStringLiteral("SELECT 1;\nSELECT 2;"));

        // Re-comment and uncomment
        sel_all = ed->textCursor();
        sel_all.select(QTextCursor::Document);
        ed->setTextCursor(sel_all);
        QVERIFY(ed->comment_selection(false).has_value());
        sel_all.select(QTextCursor::Document);
        ed->setTextCursor(sel_all);
        const auto u_res = ed->comment_selection(true);
        QVERIFY(u_res.has_value());
        QCOMPARE(ed->toPlainText(), QStringLiteral("SELECT 1;\nSELECT 2;"));

        // Large-selection (>10,000 lines) comment guard
        QStringList big_lines;
        big_lines.reserve(10005);
        for (int i = 0; i < 10005; ++i) {
            big_lines.push_back(QStringLiteral("SELECT 1;"));
        }
        tab.set_editor_text(big_lines.join(QChar('\n')));
        sel_all = ed->textCursor();
        sel_all.select(QTextCursor::Document);
        ed->setTextCursor(sel_all);
        const auto big_res = ed->comment_selection(false);
        QVERIFY(!big_res.has_value());

        // Go To Line bar (invalid feedback vs valid navigation)
        tab.set_editor_text(QStringLiteral("line1\nline2\nline3"));
        tab.show_go_to_line_bar();
        QVERIFY(tab.go_to_line_bar()->isVisible());
        tab.go_to_line_bar()->line_edit()->setText(QStringLiteral("999"));
        QVERIFY(!tab.go_to_line_bar()->navigate());
        QVERIFY(tab.go_to_line_bar()->feedback_label()->isVisible());
        tab.go_to_line_bar()->line_edit()->setText(QStringLiteral("2"));
        QVERIFY(tab.go_to_line_bar()->navigate());
        QCOMPARE(ed->textCursor().blockNumber(), 1);
        QVERIFY(!tab.go_to_line_bar()->isVisible());

        // Find / Replace bar (Match case, Whole words, Replace All single Undo)
        tab.set_editor_text(QStringLiteral("foo foobar Foo foo"));
        tab.show_replace_bar();
        auto* fr = tab.find_replace_bar();
        QVERIFY(fr->isVisible());
        fr->find_edit()->setText(QStringLiteral("foo"));
        fr->replace_edit()->setText(QStringLiteral("baz"));
        fr->match_case_check()->setChecked(true);
        fr->whole_words_check()->setChecked(true);
        fr->replace_all();
        QCOMPARE(ed->toPlainText(), QStringLiteral("baz foobar Foo baz"));
        ed->undo();
        QCOMPARE(ed->toPlainText(), QStringLiteral("foo foobar Foo foo"));

        QVERIFY(tab.handle_escape_key());
        QVERIFY(!fr->isVisible());
    }

    void query_tab_streams_results_navigates_to_errors_cancels_queries_and_guards_open_transactions() {
        struct MockState {
            std::atomic<bool> in_wait{false};
            std::atomic<bool> cancel_invoked{false};
            std::atomic<bool> open_tx{true};
            std::atomic<bool> committed{false};
        };
        auto shared = std::make_shared<MockState>();

        class MockQueryDriver final : public capybaradb::drivers::DbDriver {
        public:
            explicit MockQueryDriver(std::shared_ptr<MockState> st)
                : st_(std::move(st)) {}

            std::expected<void, capybaradb::drivers::DbError> connect(
                const capybaradb::drivers::ConnectionConfig& cfg,
                capybaradb::core::CancelToken&) override {
                cfg_ = cfg;
                connected_ = true;
                return {};
            }

            void disconnect() noexcept override { connected_ = false; }
            bool is_connected() const noexcept override { return connected_; }
            capybaradb::drivers::SessionInfo session_info() const override {
                return capybaradb::drivers::SessionInfo{
                    .engine         = cfg_.engine,
                    .host           = cfg_.host,
                    .port           = cfg_.port,
                    .server_version = "PostgreSQL 17.0 Mock",
                    .database       = cfg_.database,
                    .username       = cfg_.username,
                    .session_id     = 4242,
                };
            }

            std::expected<capybaradb::drivers::ConnectionTestResult, capybaradb::drivers::DbError> test_connection(
                const capybaradb::drivers::ConnectionConfig&,
                capybaradb::core::CancelToken&) override {
                return capybaradb::drivers::ConnectionTestResult{.database = cfg_.database, .user = cfg_.username};
            }

            std::expected<void, capybaradb::drivers::DbError> switch_database(
                std::string_view db,
                capybaradb::core::CancelToken&) override {
                cfg_.database = std::string(db);
                return {};
            }

            std::expected<capybaradb::drivers::SessionLossRisk, capybaradb::drivers::DbError> inspect_loss_risk(
                capybaradb::core::CancelToken&) override {
                return capybaradb::drivers::SessionLossRisk{.open_transaction = st_->open_tx.load()};
            }

            std::expected<void, capybaradb::drivers::DbError> commit_transaction(
                capybaradb::core::CancelToken&) override {
                st_->open_tx.store(false);
                st_->committed.store(true);
                return {};
            }

            std::expected<void, capybaradb::drivers::DbError> execute_stream(
                std::string_view sql,
                const capybaradb::drivers::ExecOptions& opts,
                capybaradb::core::QueryExecutionResult& out,
                capybaradb::core::StreamPublishSink& sink,
                capybaradb::core::CancelToken& cancel) override {
                if (sql.find("WAIT_CANCEL") != std::string_view::npos) {
                    std::unique_lock<std::mutex> lock(mu_);
                    st_->in_wait.store(true, std::memory_order_release);
                    cv_.wait(lock, [&]() {
                        return cancel.is_canceled() || st_->cancel_invoked.load(std::memory_order_acquire);
                    });
                    return std::unexpected(capybaradb::drivers::DbError{
                        .sql_state   = "57014",
                        .severity    = "ERROR",
                        .message     = "canceling statement due to user request",
                        .is_canceled = true,
                    });
                }
                if (sql.find("ERR_POS") != std::string_view::npos) {
                    return std::unexpected(capybaradb::drivers::DbError{
                        .sql_state = "42601",
                        .severity  = "ERROR",
                        .message   = "syntax error at ERR_POS",
                        .position  = 10,
                    });
                }
                out.append_message(capybaradb::core::MessageSeverity::Notice, "00000", "mock execution started");
                const std::array<capybaradb::core::ColumnSpec, 2> cols = {{
                    {"id", "int4", capybaradb::core::LogicalTypeKind::Int, false},
                    {"label", "text", capybaradb::core::LogicalTypeKind::Text, true},
                }};
                capybaradb::core::ResultSetStorage* rs = out.create_result_set(cols);
                if (rs != nullptr) {
                    capybaradb::core::ResultSetWriter writer(out, *rs, opts.max_arena_bytes, opts.max_cell_bytes);
                    for (int i = 1; i <= 35; ++i) {
                        if (writer.begin_row()) {
                            const std::string s_id = std::to_string(i);
                            const std::string s_lbl = "row_" + s_id;
                            writer.write_utf8(0, s_id.data(), s_id.size());
                            writer.write_utf8(1, s_lbl.data(), s_lbl.size());
                            (void)writer.end_row(i == 1 || i == 30);
                        }
                    }
                    writer.finish();
                    out.add_rows_affected(35, rs);
                    sink.notify_if_unscheduled();
                }
                return {};
            }

            std::expected<void, capybaradb::drivers::DbError> parse_only(
                std::string_view sql,
                capybaradb::core::CancelToken&) override {
                if (sql.find("FROMM") != std::string_view::npos) {
                    return std::unexpected(capybaradb::drivers::DbError{
                        .sql_state = "42601",
                        .severity  = "ERROR",
                        .message   = "syntax error at or near \"FROMM\"",
                        .position  = 10,
                    });
                }
                return {};
            }

            void cancel_active() noexcept override {
                st_->cancel_invoked.store(true, std::memory_order_release);
                cv_.notify_all();
            }

        private:
            std::shared_ptr<MockState>      st_;
            capybaradb::drivers::ConnectionConfig cfg_;
            bool                            connected_ = false;
            std::mutex                      mu_;
            std::condition_variable         cv_;
        };

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        capybaradb::ui::MainWindow win(tmp.path().toStdWString());
        win.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) {
            return QMessageBox::No;
        });
        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        QVERIFY(tab != nullptr);
        tab->set_driver_factory([shared](capybaradb::drivers::EngineKind) {
            return std::make_unique<MockQueryDriver>(shared);
        });

        capybaradb::drivers::ConnectionConfig cfg;
        cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        cfg.host     = "127.0.0.1";
        cfg.port     = 5432;
        cfg.database = "appdb";
        cfg.username = "postgres";

        tab->connect_async(cfg, &win.worker_pool());
        QTRY_VERIFY(tab->is_connected());

        // Results-to-Text streaming execution + Messages + row counter
        tab->set_results_destination(capybaradb::core::ResultsDestination::Text);
        tab->set_editor_text(QStringLiteral("SELECT id, label FROM t;"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(tab->results_text_view()->toPlainText().contains(QStringLiteral("35 row(s) returned")));
        QVERIFY(tab->results_text_view()->toPlainText().contains(QStringLiteral("Elapsed:")));
        QVERIFY(tab->messages_view()->toPlainText().contains(QStringLiteral("mock execution started")));
        QCOMPARE(tab->status_rows_label()->text(), QStringLiteral("35 linhas"));

        // Parse + Go to Error navigation
        tab->set_editor_text(QStringLiteral("SELECT * FROMM broken;"));
        tab->parse_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(tab->go_to_error_button()->isEnabled());
        QVERIFY(tab->messages_view()->toPlainText().contains(QStringLiteral("42601")));
        tab->navigate_to_error();
        QCOMPARE(tab->editor_widget()->textCursor().position(), 9);

        // Fast native cancellation (< 500 ms)
        shared->in_wait.store(false);
        shared->cancel_invoked.store(false);
        tab->set_editor_text(QStringLiteral("WAIT_CANCEL;"));
        tab->execute_query();
        QTRY_VERIFY(tab->is_busy());
        QTRY_VERIFY(shared->in_wait.load());

        QElapsedTimer cancel_timer;
        cancel_timer.start();
        tab->cancel_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(cancel_timer.elapsed() < 500);
        QVERIFY(shared->cancel_invoked.load());
        QCOMPARE(tab->status_message_label()->text(), QStringLiteral("Consulta cancelada pelo usuário."));

        // Open-transaction close guard: Cancel keeps tab, Yes commits and closes
        bool prompt_seen = false;
        win.set_open_tx_close_prompt([&prompt_seen](bool) {
            prompt_seen = true;
            return QMessageBox::Cancel;
        });
        QVERIFY(!win.close_active_query());
        QVERIFY(prompt_seen);
        QCOMPARE(win.query_tab_count(), 1);

        win.set_open_tx_close_prompt([](bool) {
            return QMessageBox::Yes;
        });
        QVERIFY(win.close_active_query());
        QVERIFY(shared->committed.load());
        QCOMPARE(win.query_tab_count(), 0);
    }
};

QTEST_MAIN(ShellAndEditorTest)
#include "shell_and_editor_test.moc"
