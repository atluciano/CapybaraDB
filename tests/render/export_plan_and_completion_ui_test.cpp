#include "core/completion/sql_completion.hpp"
#include "core/config/connection_profile.hpp"
#include "core/config/environment_options.hpp"
#include "core/config/query_history.hpp"
#include "core/config/query_options.hpp"
#include "core/config/query_recovery.hpp"
#include "core/plan/execution_plan.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/stream_sink.hpp"
#include "drivers/common/db_driver.hpp"
#include "ui/dialogs/export_flyout_widget.hpp"
#include "ui/dialogs/options_dialog.hpp"
#include "ui/dialogs/query_history_dialog.hpp"
#include "ui/dialogs/query_options_dialog.hpp"
#include "ui/editor/completion_popup.hpp"
#include "ui/editor/query_tab_widget.hpp"
#include "ui/editor/sql_code_editor.hpp"
#include "ui/explorer/registered_servers_panel.hpp"
#include "ui/grid/result_grid_viewport.hpp"
#include "ui/plan/execution_plan_widget.hpp"
#include "ui/shell/main_window.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <DockWidget.h>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <array>
#include <atomic>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

namespace {

constexpr const char* kSamplePgJsonPlan = R"json([
  {
    "Plan": {
      "Node Type": "Seq Scan",
      "Relation Name": "orders",
      "Schema": "public",
      "Filter": "(status = 'open'::text\u0029",
      "Total Cost": 150.0,
      "Startup Cost": 0.0,
      "Plan Rows": 10,
      "Actual Rows": 5,
      "Actual Loops": 1,
      "Actual Total Time": 4.2,
      "Rows Removed by Filter": 25000
    },
    "Planning Time": 0.2,
    "Execution Time": 4.3
  }
])json";

constexpr const char* kSampleMsSqlXmlPlan =
    "<?xml version=\"1.0\" encoding=\"utf-16\"?>\n"
    "<ShowPlanXML xmlns=\"http://schemas.microsoft.com/sqlserver/2004/07/showplan\">\n"
    "  <BatchSequence><Batch><Statements>\n"
    "    <StmtSimple StatementId=\"1\" StatementText=\"SELECT * FROM dbo.Orders WHERE CustomerId = 5\" StatementSubTreeCost=\"0.75\">\n"
    "      <QueryPlan>\n"
    "        <MissingIndexes>\n"
    "          <MissingIndexGroup Impact=\"88.0\">\n"
    "            <MissingIndex Database=\"[SalesDb]\" Schema=\"[dbo]\" Table=\"[Orders]\">\n"
    "              <ColumnGroup Usage=\"EQUALITY\"><Column Name=\"[CustomerId]\"/></ColumnGroup>\n"
    "            </MissingIndex>\n"
    "          </MissingIndexGroup>\n"
    "        </MissingIndexes>\n"
    "        <RelOp NodeId=\"0\" PhysicalOp=\"Clustered Index Scan\" LogicalOp=\"Clustered Index Scan\" EstimateRows=\"100\" EstimatedTotalSubtreeCost=\"0.75\">\n"
    "          <RunTimeInformation>\n"
    "            <RunTimeCountersPerThread Thread=\"0\" ActualRows=\"3\" ActualExecutions=\"1\" ActualElapsedms=\"2\"/>\n"
    "          </RunTimeInformation>\n"
    "          <IndexScan><Object Database=\"[SalesDb]\" Schema=\"[dbo]\" Table=\"[Orders]\" Index=\"[PK_Orders]\"/></IndexScan>\n"
    "        </RelOp>\n"
    "      </QueryPlan>\n"
    "    </StmtSimple>\n"
    "  </Statements></Batch></BatchSequence>\n"
    "</ShowPlanXML>";

class MockM5Driver final : public capybaradb::drivers::DbDriver {
public:
    std::atomic<int> execute_calls{0};

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
            .server_version = (cfg_.engine == capybaradb::drivers::EngineKind::SqlServer)
                                  ? "Microsoft SQL Server 2022"
                                  : "PostgreSQL 17.2",
            .database       = cfg_.database,
            .username       = cfg_.username,
            .session_id     = 555,
        };
    }
    std::expected<capybaradb::drivers::ConnectionTestResult, capybaradb::drivers::DbError> test_connection(
        const capybaradb::drivers::ConnectionConfig&,
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::ConnectionTestResult{};
    }
    std::expected<void, capybaradb::drivers::DbError> switch_database(
        std::string_view db,
        capybaradb::core::CancelToken&) override {
        cfg_.database = std::string(db);
        return {};
    }
    std::expected<capybaradb::drivers::SessionLossRisk, capybaradb::drivers::DbError> inspect_loss_risk(
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::SessionLossRisk{};
    }
    std::expected<void, capybaradb::drivers::DbError> commit_transaction(capybaradb::core::CancelToken&) override {
        return {};
    }

    std::expected<void, capybaradb::drivers::DbError> execute_stream(
        std::string_view sql,
        const capybaradb::drivers::ExecOptions& opts,
        capybaradb::core::QueryExecutionResult& out,
        capybaradb::core::StreamPublishSink& sink,
        capybaradb::core::CancelToken&) override {
        execute_calls.fetch_add(1, std::memory_order_relaxed);
        const std::array<capybaradb::core::ColumnSpec, 3> cols = {{
            {"id", "int4", capybaradb::core::LogicalTypeKind::Int, false},
            {"customer", "text", capybaradb::core::LogicalTypeKind::Text, true},
            {"amount", "numeric", capybaradb::core::LogicalTypeKind::Numeric, false},
        }};
        auto* rs = out.create_result_set(cols);
        if (rs != nullptr) {
            capybaradb::core::ResultSetWriter w(out, *rs, opts.max_arena_bytes, opts.max_cell_bytes);
            const std::array<std::tuple<const char*, const char*, const char*>, 3> rows = {{
                {"10", "Alice", "120.50"},
                {"2",  nullptr, "15.00"},
                {"30", "Jos\xC3\xA9", "999.99"},
            }};
            for (const auto& [a, b, c] : rows) {
                (void)w.begin_row();
                w.write_utf8(0, a, std::strlen(a));
                if (b) w.write_utf8(1, b, std::strlen(b)); else w.write_null(1);
                w.write_utf8(2, c, std::strlen(c));
                (void)w.end_row(false);
            }
            w.finish();
        }
        if (opts.include_actual_plan) {
            capybaradb::core::RawExecutionPlan raw;
            raw.source_kind = (cfg_.engine == capybaradb::drivers::EngineKind::SqlServer)
                                  ? capybaradb::core::PlanSourceKind::SqlServerXml
                                  : capybaradb::core::PlanSourceKind::PostgreSqlJson;
            raw.raw_text    = (cfg_.engine == capybaradb::drivers::EngineKind::SqlServer)
                                  ? std::string(kSampleMsSqlXmlPlan)
                                  : std::string(kSamplePgJsonPlan);
            raw.analyzed    = true;
            raw.query_text  = std::string(sql);
            out.add_actual_plan(std::move(raw));
        }
        sink.notify_if_unscheduled();
        return {};
    }

    std::expected<void, capybaradb::drivers::DbError> parse_only(
        std::string_view,
        capybaradb::core::CancelToken&) override {
        return {};
    }

    std::expected<std::vector<capybaradb::core::RawExecutionPlan>, capybaradb::drivers::DbError> explain_plan(
        std::string_view sql,
        bool analyze,
        uint32_t,
        capybaradb::core::CancelToken&) override {
        capybaradb::core::RawExecutionPlan raw;
        raw.source_kind = (cfg_.engine == capybaradb::drivers::EngineKind::SqlServer)
                              ? capybaradb::core::PlanSourceKind::SqlServerXml
                              : capybaradb::core::PlanSourceKind::PostgreSqlJson;
        raw.raw_text    = (cfg_.engine == capybaradb::drivers::EngineKind::SqlServer)
                              ? std::string(kSampleMsSqlXmlPlan)
                              : std::string(kSamplePgJsonPlan);
        raw.analyzed    = analyze;
        raw.query_text  = std::string(sql);
        return std::vector<capybaradb::core::RawExecutionPlan>{std::move(raw)};
    }

    void cancel_active() noexcept override {}

private:
    capybaradb::drivers::ConnectionConfig cfg_;
    bool connected_ = false;
};

} // namespace

class ExportPlanAndCompletionUiTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(capybaradb::ui::theme::apply_classic_blue_theme(*qApp));
    }

    void export_flyout_streams_csv_and_json_and_routes_results_to_text_and_rpt_files() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto state_path = std::filesystem::path(tmp.path().toStdWString());

        capybaradb::ui::MainWindow win(state_path);
        win.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) { return QMessageBox::No; });
        win.resize(1200, 750);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        QVERIFY(tab != nullptr);
        std::shared_ptr<std::atomic<int>> exec_counter = std::make_shared<std::atomic<int>>(0);
        tab->set_driver_factory([exec_counter](capybaradb::drivers::EngineKind) {
            auto drv = std::make_unique<MockM5Driver>();
            return drv;
        });

        capybaradb::drivers::ConnectionConfig cfg;
        cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        cfg.host     = "127.0.0.1";
        cfg.port     = 5432;
        cfg.database = "sales";
        cfg.username = "postgres";

        tab->connect_async(cfg, &win.worker_pool());
        QTRY_VERIFY(tab->is_connected());

        // 1. Execute to Grid -> Export CSV via ExportFlyoutWidget (sorted order + BOM + semicolon)
        tab->set_results_destination(capybaradb::core::ResultsDestination::Grid);
        tab->set_editor_text(QStringLiteral("SELECT id, customer, amount FROM orders;"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(tab->export_csv_button()->isEnabled());

        // Sort by id ascending (2, 10, 30) and select row 0 (id=2)
        tab->result_grid()->view_state()->toggle_sort(*tab->result_grid()->storage(), 0);
        tab->result_grid()->view_state()->selection.select_single_cell(0, 0);
        emit tab->result_grid()->selection_changed();

        const QString csv_out = tmp.path() + QStringLiteral("/exported.csv");
        tab->set_export_file_picker([csv_out](capybaradb::core::ExportFormat) { return csv_out; });

        tab->export_flyout_button()->setChecked(true);
        QVERIFY(tab->export_flyout()->isVisible());
        tab->export_flyout()->format_combo()->setCurrentIndex(0);    // CSV
        tab->export_flyout()->scope_combo()->setCurrentIndex(1);     // Selected rows
        tab->export_flyout()->delimiter_combo()->setCurrentIndex(1); // Semicolon
        tab->export_flyout()->bom_checkbox()->setChecked(true);
        tab->export_flyout()->export_button()->click();

        QTRY_VERIFY(!tab->is_exporting());
        QVERIFY(QFile::exists(csv_out));
        {
            QFile f(csv_out);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray bytes = f.readAll();
            QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
            QVERIFY(bytes.contains("\"2\";\\N;\"15.00\"\r\n"));
            QVERIFY(!bytes.contains("\"10\"")); // Only selected row exported
        }

        // 2. Export JSON via Save Results As context-menu signal (all rows)
        const QString json_out = tmp.path() + QStringLiteral("/exported.json");
        tab->set_export_file_picker([json_out](capybaradb::core::ExportFormat) { return json_out; });
        tab->export_flyout()->format_combo()->setCurrentIndex(1); // JSON
        tab->export_flyout()->scope_combo()->setCurrentIndex(0);  // All displayed rows
        emit tab->result_grid()->save_results_requested();
        QTRY_VERIFY(!tab->is_exporting());
        {
            QFile f(json_out);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const auto j = nlohmann::json::parse(f.readAll().toStdString());
            QCOMPARE(j["rows"].size(), 3u);
            QCOMPARE(j["rows"][0][0].get<std::string>(), std::string("2"));
            QVERIFY(j["rows"][0][1].is_null());
        }

        // 3. Results to Text (Ctrl+T) with custom delimiter + include query via QueryOptions
        capybaradb::core::QueryOptions qopts = tab->query_options();
        qopts.text_format        = capybaradb::core::TextOutputFormat::CustomDelimited;
        qopts.custom_delimiter   = '|';
        qopts.text_include_query = true;
        tab->set_query_options(qopts);
        tab->set_results_destination(capybaradb::core::ResultsDestination::Text);
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        const QString txt_out = tab->results_text_view()->toPlainText();
        QVERIFY(txt_out.contains(QStringLiteral("SELECT id, customer, amount FROM orders;")));
        QVERIFY(txt_out.contains(QStringLiteral("id|customer|amount")));
        QVERIFY(txt_out.contains(QStringLiteral("2|NULL|15.00")));
        QVERIFY(txt_out.contains(QStringLiteral("(3 row(s) returned)")));

        // 4. Results to File (Ctrl+Shift+F): canceling file picker aborts cleanly without executing
        tab->set_results_destination(capybaradb::core::ResultsDestination::File);
        tab->set_results_file_picker([]() { return QString{}; });
        tab->execute_query();
        QVERIFY(!tab->is_busy());
        QVERIFY(tab->status_message_label()->text().contains(QStringLiteral("No results file chosen")));

        // Choosing a .rpt path streams UTF-8 BOM + CRLF and reports in Messages
        const QString rpt_path = tmp.path() + QStringLiteral("/out.rpt");
        tab->set_results_file_picker([rpt_path]() { return rpt_path; });
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(QFile::exists(rpt_path));
        QVERIFY(tab->messages_view()->toPlainText().contains(rpt_path));
        {
            QFile f(rpt_path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray bytes = f.readAll();
            QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
            QVERIFY(bytes.contains("id|customer|amount\r\n"));
            QVERIFY(bytes.contains("(3 row(s) returned)\r\n"));
        }
    }

    void execution_plan_widget_renders_postgres_and_mssql_plans_finds_nodes_and_scripts_missing_indexes() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        capybaradb::ui::MainWindow win(std::filesystem::path(tmp.path().toStdWString()));
        win.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) { return QMessageBox::No; });
        win.resize(1200, 750);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        tab->set_driver_factory([](capybaradb::drivers::EngineKind) { return std::make_unique<MockM5Driver>(); });

        capybaradb::drivers::ConnectionConfig pg_cfg;
        pg_cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        pg_cfg.host     = "127.0.0.1";
        pg_cfg.port     = 5432;
        pg_cfg.database = "appdb";
        pg_cfg.username = "postgres";

        tab->connect_async(pg_cfg, &win.worker_pool());
        QTRY_VERIFY(tab->is_connected());
        QVERIFY(tab->estimated_plan_button()->isEnabled());
        QVERIFY(tab->analyze_plan_button()->isEnabled());

        // Estimated Plan (PostgreSQL JSON)
        tab->set_editor_text(QStringLiteral("SELECT * FROM public.orders WHERE status = 'open';"));
        tab->run_estimated_plan();
        QTRY_VERIFY(!tab->is_busy());
        auto* plan = tab->execution_plan_widget();
        QVERIFY(plan != nullptr);
        QCOMPARE(plan->statements().size(), 1u);
        QVERIFY(plan->operator_count() >= 1);
        QVERIFY(plan->selected_operator() != nullptr);
        QVERIFY(plan->properties_table()->rowCount() > 0);
        QVERIFY(plan->missing_index_button()->isEnabled());
        QCOMPARE(tab->output_tab_bar()->currentIndex(), 2);

        // Zoom + Find Node
        plan->set_zoom(1.5);
        QVERIFY(qAbs(plan->zoom() - 1.5) < 0.01);
        plan->open_find_bar();
        QVERIFY(plan->is_find_bar_visible());
        plan->find_value_edit()->setText(QStringLiteral("Seq"));
        QVERIFY(plan->find_next_node(true));

        // Missing Index Details opens commented script in a new query tab
        const int before_tabs = win.query_tab_count();
        plan->request_missing_index_script();
        QCOMPARE(win.query_tab_count(), before_tabs + 1);
        QVERIFY(win.active_query()->editor_text().contains(QStringLiteral("CREATE INDEX")));
        QVERIFY(win.active_query()->editor_text().contains(QStringLiteral("/*")));

        // Show Execution Plan In New Tab opens a standalone plan dock tab
        emit plan->show_in_new_tab_requested(plan->statements());
        QCOMPARE(win.plan_tab_count(), 1);

        // Save Plan As + Open Execution Plan… round-trip
        const QString saved_plan = tmp.path() + QStringLiteral("/pg_plan.json");
        plan->set_save_file_picker([saved_plan](const QString&) { return saved_plan; });
        plan->save_selected_plan();
        QVERIFY(QFile::exists(saved_plan));

        win.set_open_plan_file_picker([saved_plan]() { return saved_plan; });
        win.open_execution_plan_file();
        QCOMPARE(win.plan_tab_count(), 2);

        // Bring query tab back to front so its editor and execution plan widget are active
        if (!win.query_docks().empty()) {
            win.query_docks().front()->setAsCurrentTab();
        }

        // Editing the query marks plan stale
        tab->set_editor_text(QStringLiteral("SELECT 42;"));
        QVERIFY(plan->stale_label()->isVisible());

        // Analyze Plan confirmation hook (declined -> stays idle; accepted -> runs)
        int confirm_hits = 0;
        tab->set_analyze_confirm_handler([&](const QString&) {
            ++confirm_hits;
            return false;
        });
        tab->run_analyze_plan(false);
        QCOMPARE(confirm_hits, 1);
        QVERIFY(!tab->is_busy());

        // Switch to SQL Server + Include Actual Execution Plan (Ctrl+M)
        capybaradb::drivers::ConnectionConfig ms_cfg = pg_cfg;
        ms_cfg.engine = capybaradb::drivers::EngineKind::SqlServer;
        ms_cfg.port   = 1433;
        tab->connect_async(ms_cfg, &win.worker_pool());
        QTRY_VERIFY(tab->session_info().engine == capybaradb::drivers::EngineKind::SqlServer);

        tab->set_include_actual_plan(true);
        tab->set_editor_text(QStringLiteral("SELECT * FROM dbo.Orders WHERE CustomerId = 5;"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QCOMPARE(plan->statements().size(), 1u);
        QCOMPARE(plan->statements()[0].root.children.size(), 1u);
        QCOMPARE(plan->statements()[0].root.children[0].title, std::string("Clustered Index Scan"));
        QVERIFY(!plan->statements()[0].missing_indexes.empty());
    }

    void completion_popup_filters_accent_insensitively_and_honors_tab_and_enter_commit_rules() {
        capybaradb::ui::QueryTabWidget tab;
        tab.resize(900, 600);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));

        capybaradb::core::SqlCompletionCatalog cat;
        cat.is_tsql     = false;
        cat.database    = "appdb";
        cat.search_path = {"public"};
        cat.schemas     = {"public"};
        cat.relations   = {{"public", "Usu\xC3\xA1rios", false}, {"public", "orders", false}};
        cat.set_relation_columns("public", "orders", {{"id", "int4", false}, {"total", "numeric", false}});
        tab.set_completion_catalog_for_test(cat);

        // 1. Ctrl+Space accent-insensitive completion: "SELECT * FROM usuar" -> Tab accepts "Usuários"
        tab.set_editor_text(QStringLiteral("SELECT * FROM usuar"));
        QTextCursor cur = tab.editor_widget()->textCursor();
        cur.movePosition(QTextCursor::End);
        tab.editor_widget()->setTextCursor(cur);
        tab.focus_editor();

        tab.trigger_intellisense(false);
        auto* pop = tab.completion_popup();
        QVERIFY(pop != nullptr);
        QVERIFY(pop->is_open());
        QVERIFY(pop->visible_count() >= 1);
        QVERIFY(!pop->has_explicit_choice());

        // Press Tab -> accepts first item
        QKeyEvent tab_ev(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(tab.editor_widget(), &tab_ev);
        QVERIFY(!pop->is_open());
        QVERIFY(tab.editor_text().contains(QStringLiteral("Usu\u00E1rios")));

        // 2. Default Enter rule: without arrow navigation, Enter closes popup & inserts newline
        tab.set_editor_text(QStringLiteral("SELECT o. FROM orders o"));
        cur = tab.editor_widget()->textCursor();
        cur.setPosition(9); // right after "o."
        tab.editor_widget()->setTextCursor(cur);
        tab.trigger_intellisense(false);
        QVERIFY(pop->is_open());
        QVERIFY(!pop->has_explicit_choice());

        QKeyEvent enter_ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(tab.editor_widget(), &enter_ev);
        QVERIFY(!pop->is_open());
        QVERIFY(tab.editor_text().contains(u'\n'));

        // 3. With explicit arrow selection (Down), Enter accepts the chosen column
        tab.set_editor_text(QStringLiteral("SELECT o. FROM orders o"));
        cur = tab.editor_widget()->textCursor();
        cur.setPosition(9);
        tab.editor_widget()->setTextCursor(cur);
        tab.trigger_intellisense(false);
        QVERIFY(pop->is_open());
        QKeyEvent down_ev(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(tab.editor_widget(), &down_ev);
        QVERIFY(pop->has_explicit_choice());
        QApplication::sendEvent(tab.editor_widget(), &enter_ev);
        QVERIFY(!pop->is_open());
        QVERIFY(tab.editor_text().contains(QStringLiteral("o.total")) ||
                tab.editor_text().contains(QStringLiteral("o.id")));

        // 4. Disabling IntelliSense closes popup and blocks triggers
        tab.set_intellisense_enabled(false);
        tab.trigger_intellisense(false);
        QVERIFY(!pop->is_open());
    }

    void options_registered_servers_query_history_and_crash_recovery_workflows_round_trip_state() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto state_path = std::filesystem::path(tmp.path().toStdWString());

        // 1. OptionsDialog round-trip to environment.json
        {
            capybaradb::ui::OptionsDialog opt_dlg(state_path);
            opt_dlg.select_top_edit()->setText(QStringLiteral("500"));
            opt_dlg.history_days_edit()->setText(QStringLiteral("45"));
            opt_dlg.query_pages()->nocount_box()->setChecked(true);
            for (QPushButton* btn : opt_dlg.findChildren<QPushButton*>()) {
                if (btn->text() == QStringLiteral("OK")) {
                    btn->click();
                    break;
                }
            }
            QCOMPARE(opt_dlg.result(), static_cast<int>(QDialog::Accepted));
            const auto loaded = capybaradb::core::load_environment_options(state_path / "environment.json");
            QCOMPARE(loaded.normalized_select_top(), 500);
            QCOMPARE(loaded.query_history.retention_days, 45);
            QVERIFY(loaded.default_query_options.nocount);
        }
        {
            // Verify via QueryOptionsDialog round-trip as well
            capybaradb::core::QueryOptions initial;
            capybaradb::ui::QueryOptionsDialog qdlg(initial);
            qdlg.pages_widget()->timeout_edit()->setText(QStringLiteral("15"));
            qdlg.pages_widget()->json_link_box()->setChecked(false);
            qdlg.pages_widget()->text_format_combo()->setCurrentIndex(2); // Tab delimited
            qdlg.findChild<QPushButton*>();
            // Call OK button click so on_accept() runs validation + collection
            for (QPushButton* btn : qdlg.findChildren<QPushButton*>()) {
                if (btn->text() == QStringLiteral("OK")) {
                    btn->click();
                    break;
                }
            }
            QCOMPARE(qdlg.result(), static_cast<int>(QDialog::Accepted));
            QCOMPARE(qdlg.options().execution_timeout_seconds, 15);
            QVERIFY(!qdlg.options().display_json_as_link);
            QCOMPARE(qdlg.options().text_format, capybaradb::core::TextOutputFormat::TabDelimited);
        }

        // 2. RegisteredServersPanel (Ctrl+Alt+G)
        {
            capybaradb::core::ConnectionProfile p;
            p.id       = capybaradb::core::generate_uuid();
            p.name     = "SyntheticDB";
            p.engine   = "PostgreSql";
            p.host     = "127.0.0.1";
            p.port     = 5432;
            p.database = "appdb";
            p.username = "postgres";
            p.group    = "QA";
            QVERIFY(capybaradb::core::save_connection_profile(state_path / "connections.json", p));
        }

        capybaradb::ui::MainWindow win(state_path);
        win.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) { return QMessageBox::No; });
        win.resize(1200, 750);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        win.show_registered_servers();
        QVERIFY(win.registered_servers_dock() != nullptr);
        QVERIFY(!win.registered_servers_dock()->isClosed());
        QVERIFY(win.registered_servers_panel()->create_group(QStringLiteral("Analytics")));
        QVERIFY(win.registered_servers_panel()->tree_widget()->topLevelItemCount() >= 2);

        // 3. Query execution automatically records encrypted Query History -> inspect in QueryHistoryDialog
        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        tab->set_driver_factory([](capybaradb::drivers::EngineKind) { return std::make_unique<MockM5Driver>(); });
        capybaradb::drivers::ConnectionConfig cfg;
        cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        cfg.host     = "127.0.0.1";
        cfg.port     = 5432;
        cfg.database = "appdb";
        cfg.username = "postgres";
        tab->connect_async(cfg, &win.worker_pool());
        QTRY_VERIFY(tab->is_connected());

        tab->set_editor_text(QStringLiteral("SELECT id FROM history_probe_table;"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());

        const std::filesystem::path hpath = state_path / "query-history.dat";
        QTRY_VERIFY(!capybaradb::core::load_query_history_groups(hpath).empty());

        capybaradb::ui::QueryHistoryDialog hdlg(state_path);
        QVERIFY(hdlg.entries_tree()->topLevelItemCount() >= 1);
        hdlg.entries_tree()->setCurrentItem(hdlg.entries_tree()->topLevelItem(0));
        QVERIFY(hdlg.sql_preview()->toPlainText().contains(QStringLiteral("history_probe_table")));
        hdlg.toggle_pin_selected();
        QVERIFY(capybaradb::core::load_query_history_groups(hpath)[0].pinned);

        QSignalSpy open_spy(&hdlg, &capybaradb::ui::QueryHistoryDialog::open_in_new_query_requested);
        hdlg.open_selected();
        QCOMPARE(open_spy.count(), 1);

        // 4. Encrypted Draft Recovery across simulated crash/restart
        tab->set_editor_text(QStringLiteral("SELECT 'unsaved_recovered_query';"));
        tab->set_word_wrap(true);
        tab->set_show_white_space(true);
        capybaradb::core::QueryOptions topts = tab->query_options();
        topts.execution_timeout_seconds = 77;
        tab->set_query_options(topts);
        win.checkpoint_all_drafts_sync();

        const std::filesystem::path rec_dir = state_path / "recovery";
        QCOMPARE(capybaradb::core::list_recovery_draft_ids(rec_dir).size(), 1u);

        // Simulate new MainWindow startup recovering the draft
        capybaradb::ui::MainWindow win_restarted(state_path);
        win_restarted.set_dirty_close_prompt([](capybaradb::ui::QueryTabWidget*) { return QMessageBox::No; });
        win_restarted.set_recovery_prompt([](int count) { return count == 1; });
        QVERIFY(win_restarted.check_draft_recovery());
        QCOMPARE(win_restarted.query_tab_count(), 1);
        capybaradb::ui::QueryTabWidget* rec = win_restarted.active_query();
        QVERIFY(rec != nullptr);
        QCOMPARE(rec->editor_text(), QStringLiteral("SELECT 'unsaved_recovered_query';"));
        QVERIFY(rec->is_dirty());
        QVERIFY(rec->word_wrap());
        QVERIFY(rec->show_white_space());
        QCOMPARE(rec->query_options().execution_timeout_seconds, 77);

        // Clean close deletes the recovered draft from disk
        QVERIFY(win_restarted.close_active_query());
        QVERIFY(capybaradb::core::list_recovery_draft_ids(rec_dir).empty());
    }
};

QTEST_MAIN(ExportPlanAndCompletionUiTest)
#include "export_plan_and_completion_ui_test.moc"
