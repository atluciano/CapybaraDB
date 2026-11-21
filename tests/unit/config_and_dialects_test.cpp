#include "core/config/connection_profile.hpp"
#include "core/config/connection_uri_parser.hpp"
#include "core/config/environment_options.hpp"
#include "core/config/state_paths.hpp"
#include "core/config/utf_convert.hpp"
#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/cancel_token.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "core/threading/db_session_worker.hpp"
#include "core/threading/stream_sink.hpp"
#include "dialects/sql_dialect.hpp"
#include "dialects/sql_placeholder_scanner.hpp"
#include "drivers/common/catalog_provider.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"
#include "drivers/mssql/mssql_batch_splitter.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

class TempDirGuard final {
public:
    explicit TempDirGuard(const char* name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("capybaradb_test_" + std::string(name) + "_" + std::to_string(ticks));
        std::filesystem::create_directories(path_);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("EnvironmentOptionsStore loads defaults and persists updates atomically", "[core][config][environment]") {
    TempDirGuard tmp("env_roundtrip");
    const std::filesystem::path file = tmp.path() / "environment.json";

    // Defaults when file is absent
    const capybaradb::core::EnvironmentDocument initial = capybaradb::core::load_environment_options(file);
    REQUIRE(initial.version == 1);
    REQUIRE(initial.restore_window_placement);
    REQUIRE_FALSE(initial.window.has_value());
    REQUIRE(initial.normalized_select_top() == 1000);
    REQUIRE(initial.normalized_edit_top() == 200);
    REQUIRE(initial.normalized_default_results() == capybaradb::core::ResultsDestination::Grid);

    capybaradb::core::EnvironmentDocument doc = initial;
    doc.restore_window_placement = true;
    doc.select_top_rows = 2500;
    doc.edit_top_rows = 50;
    doc.default_results_destination = capybaradb::core::ResultsDestination::Text;
    doc.window = capybaradb::core::WindowPlacement{120, 80, 1280, 760, true, 340};
    const bool ok = capybaradb::core::save_environment_options(file, doc);
    REQUIRE(ok);
    REQUIRE(std::filesystem::exists(file));

    // Confirm zero leftover .tmp files
    for (const auto& entry : std::filesystem::directory_iterator(tmp.path())) {
        REQUIRE(entry.path().extension() != ".tmp");
    }

    const capybaradb::core::EnvironmentDocument loaded = capybaradb::core::load_environment_options(file);
    REQUIRE(loaded.version == 1);
    REQUIRE(loaded.restore_window_placement);
    REQUIRE(loaded.normalized_select_top() == 2500);
    REQUIRE(loaded.normalized_edit_top() == 50);
    REQUIRE(loaded.normalized_default_results() == capybaradb::core::ResultsDestination::Text);
    REQUIRE(loaded.window.has_value());
    REQUIRE(loaded.window->left == 120);
    REQUIRE(loaded.window->top == 80);
    REQUIRE(loaded.window->width == 1280);
    REQUIRE(loaded.window->height == 760);
    REQUIRE(loaded.window->maximized);
    REQUIRE(loaded.window->explorer_width == 340);
}

TEST_CASE("EnvironmentOptionsStore falls back to defaults on malformed JSON or out-of-range values", "[core][config][environment]") {
    TempDirGuard tmp("env_invalid");
    const std::filesystem::path file = tmp.path() / "environment.json";

    {
        std::ofstream out(file, std::ios::binary);
        out << "{ this is not valid json !!!";
    }
    const capybaradb::core::EnvironmentDocument corrupt = capybaradb::core::load_environment_options(file);
    REQUIRE(corrupt.version == 1);
    REQUIRE_FALSE(corrupt.window.has_value());
    REQUIRE(corrupt.normalized_select_top() == 1000);
    REQUIRE(corrupt.normalized_edit_top() == 200);

    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version":1,"select_top_rows":-5,"edit_top_rows":0,"default_results_destination":99})";
    }
    const capybaradb::core::EnvironmentDocument out_of_range = capybaradb::core::load_environment_options(file);
    REQUIRE(out_of_range.normalized_select_top() == 1000);
    REQUIRE(out_of_range.normalized_edit_top() == 200);
    REQUIRE(out_of_range.normalized_default_results() == capybaradb::core::ResultsDestination::Grid);

    capybaradb::core::EnvironmentDocument doc_width;
    doc_width.window = capybaradb::core::WindowPlacement{0, 0, 1440, 900, false, 1400};
    REQUIRE(doc_width.normalized_explorer_width() == 300);
    doc_width.window->explorer_width = 50;
    REQUIRE(doc_width.normalized_explorer_width() == 300);
    doc_width.window->explorer_width = 340;
    REQUIRE(doc_width.normalized_explorer_width() == 340);
}

TEST_CASE("ExplorerToolbarStore deduplicates and persists hidden toolbar action IDs atomically", "[core][config][toolbar]") {
    TempDirGuard tmp("toolbar");
    const std::filesystem::path file = tmp.path() / "explorer-toolbar.json";

    REQUIRE(capybaradb::core::load_explorer_hidden_toolbar_ids(file).empty());

    const std::vector<std::string> ids = {"Stop", "Connect", "Stop"};
    REQUIRE(capybaradb::core::save_explorer_hidden_toolbar_ids(file, ids));

    const std::vector<std::string> loaded = capybaradb::core::load_explorer_hidden_toolbar_ids(file);
    REQUIRE(loaded == std::vector<std::string>{"Connect", "Stop"});

    for (const auto& entry : std::filesystem::directory_iterator(tmp.path())) {
        REQUIRE(entry.path().extension() != ".tmp");
    }
}

TEST_CASE("WindowPlacement visibility check and work-area clamping prevent offscreen windows", "[core][config][window]") {
    const std::vector<capybaradb::core::ScreenRect> screens = {
        {0, 0, 1920, 1080},
    };

    REQUIRE(capybaradb::core::is_placement_visible_on_screens(
        capybaradb::core::WindowPlacement{100, 100, 1200, 800, false, 300}, screens));
    REQUIRE_FALSE(capybaradb::core::is_placement_visible_on_screens(
        capybaradb::core::WindowPlacement{3000, 2000, 1200, 800, false, 300}, screens));
    REQUIRE_FALSE(capybaradb::core::is_placement_visible_on_screens(
        capybaradb::core::WindowPlacement{100, 100, 120, 80, false, 300}, screens));

    const capybaradb::core::ScreenRect fitted = capybaradb::core::fit_bounds_to_work_area(
        capybaradb::core::ScreenRect{-50, -50, 1440, 900},
        capybaradb::core::ScreenRect{0, 0, 1280, 720});
    REQUIRE(fitted.width <= 1280 - 16);
    REQUIRE(fitted.height <= 720 - 16);
    REQUIRE(fitted.left >= 8);
    REQUIRE(fitted.top >= 8);
}

TEST_CASE("SecretStore encrypts secrets with GUID entropy and transcodes UTF-8 and UTF-16 symmetrically", "[core][config][secrets]") {
    const auto bytes = capybaradb::core::parse_guid_bytes("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(bytes.has_value());
    REQUIRE((*bytes)[0] == 0x33);
    REQUIRE((*bytes)[1] == 0x22);
    REQUIRE((*bytes)[2] == 0x11);
    REQUIRE((*bytes)[3] == 0x00);
    REQUIRE((*bytes)[4] == 0x55);
    REQUIRE((*bytes)[5] == 0x44);
    REQUIRE((*bytes)[6] == 0x77);
    REQUIRE((*bytes)[7] == 0x66);
    REQUIRE((*bytes)[8] == 0x88);
    REQUIRE((*bytes)[15] == 0xff);

    const std::string uuid = capybaradb::core::generate_uuid();
    REQUIRE(capybaradb::core::parse_guid_bytes(uuid).has_value());

    const std::string cipher = capybaradb::core::protect_secret("p@ssw0rd!çãõ", uuid);
    REQUIRE_FALSE(cipher.empty());
    REQUIRE(cipher.find("p@ssw0rd") == std::string::npos);

    const auto plain = capybaradb::core::unprotect_secret(cipher, uuid);
    REQUIRE(plain.has_value());
    REQUIRE(*plain == "p@ssw0rd!çãõ");

    // Different GUID must fail entropy validation
    const std::string other_uuid = capybaradb::core::generate_uuid();
    REQUIRE_FALSE(capybaradb::core::unprotect_secret(cipher, other_uuid).has_value());

    const std::string utf8_sample = "Olá PostgreSQL 🚀";
    const std::u16string u16 = capybaradb::core::utf8_to_utf16(utf8_sample);
    REQUIRE(capybaradb::core::utf16_to_utf8(u16) == utf8_sample);
}

TEST_CASE("ConnectionProfileStore persists encrypted profiles in PascalCase JSON and sorts case-insensitively", "[core][config][profiles]") {
    TempDirGuard tmp("conn_profiles");
    const std::filesystem::path file = tmp.path() / "connections.json";

    capybaradb::core::ConnectionProfile p1;
    p1.id       = capybaradb::core::generate_uuid();
    p1.name     = "BetaPG";
    p1.engine   = "PostgreSql";
    p1.host     = "10.0.0.5";
    p1.port     = 5432;
    p1.database = "appdb";
    p1.username = "pguser";
    p1.ssl_mode = "VerifyFull";
    p1.set_password("super_secret_pw", true);

    capybaradb::core::ConnectionProfile p2;
    p2.id        = capybaradb::core::generate_uuid();
    p2.name      = "AlphaSQL";
    p2.engine    = "SqlServer";
    p2.host      = "sql.internal";
    p2.port      = 1433;
    p2.database  = "master";
    p2.username  = "";
    p2.auth_mode = "Integrated";
    p2.ssl_mode  = "Prefer";

    REQUIRE(capybaradb::core::save_connection_profile(file, p1));
    REQUIRE(capybaradb::core::save_connection_profile(file, p2));

    // Verify JSON uses PascalCase keys and stores no plaintext password
    {
        std::ifstream in(file, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(raw.find("super_secret_pw") == std::string::npos);
        const nlohmann::json j = nlohmann::json::parse(raw);
        REQUIRE(j["Version"].get<int>() == 1);
        REQUIRE(j["Profiles"].size() == 2);
    }

    // Case-insensitive sort puts AlphaSQL before BetaPG
    const auto loaded = capybaradb::core::load_connection_profiles(file);
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].name == "AlphaSQL");
    REQUIRE(loaded[1].name == "BetaPG");
    REQUIRE(loaded[1].reveal_password() == std::optional<std::string>{"super_secret_pw"});

    REQUIRE(capybaradb::core::remove_connection_profile(file, p1.id));
    REQUIRE(capybaradb::core::load_connection_profiles(file).size() == 1);

    // Validation checks
    capybaradb::core::ConnectionProfile bad = p1;
    bad.port = 0;
    REQUIRE_FALSE(bad.validate().empty());

    const std::filesystem::path prefs_file = tmp.path() / "connect-dialog.json";
    capybaradb::core::ConnectionDialogPreferences prefs;
    prefs.last_profile = p2.id;
    prefs.width        = 510;
    prefs.height       = 700;
    REQUIRE(capybaradb::core::save_connection_dialog_preferences(prefs_file, prefs));
    const auto prefs_loaded = capybaradb::core::load_connection_dialog_preferences(prefs_file);
    REQUIRE(prefs_loaded.last_profile == std::optional<std::string>{p2.id});
    REQUIRE(prefs_loaded.width == std::optional<double>{510.0});
    REQUIRE(prefs_loaded.height == std::optional<double>{700.0});
}

TEST_CASE("DriverUiDescriptor constexpr metadata, SQLite ConnectionProfile, and ConnectionUriParser parse engine connection strings", "[core][config][profiles][uri]") {
    using namespace capybaradb::drivers;
    using namespace capybaradb::core;

    constexpr auto all_descs = all_driver_ui_descriptors();
    STATIC_REQUIRE(all_descs.size() == 3);

    constexpr const auto& pg_desc = driver_ui_descriptor(EngineKind::PostgreSql);
    STATIC_REQUIRE(pg_desc.storage_kind == ConnectionStorageKind::NetworkServer);
    STATIC_REQUIRE(pg_desc.default_port == 5432);
    STATIC_REQUIRE(pg_desc.default_database == "postgres");
    STATIC_REQUIRE(pg_desc.default_username == "postgres");
    STATIC_REQUIRE(!pg_desc.supports_integrated_auth);
    STATIC_REQUIRE(pg_desc.supports_tls);
    STATIC_REQUIRE(pg_desc.supports_root_cert);

    constexpr const auto& ms_desc = driver_ui_descriptor(EngineKind::SqlServer);
    STATIC_REQUIRE(ms_desc.storage_kind == ConnectionStorageKind::NetworkServer);
    STATIC_REQUIRE(ms_desc.default_port == 1433);
    STATIC_REQUIRE(ms_desc.default_database == "master");
    STATIC_REQUIRE(ms_desc.default_username == "sa");
    STATIC_REQUIRE(ms_desc.supports_integrated_auth);
    STATIC_REQUIRE(ms_desc.supports_tls);
    STATIC_REQUIRE(!ms_desc.supports_root_cert);

    constexpr const auto& sq_desc = driver_ui_descriptor(EngineKind::Sqlite);
    STATIC_REQUIRE(sq_desc.storage_kind == ConnectionStorageKind::LocalFile);
    STATIC_REQUIRE(sq_desc.default_port == 0);
    STATIC_REQUIRE(sq_desc.default_database == ":memory:");
    STATIC_REQUIRE(sq_desc.default_username.empty());
    STATIC_REQUIRE(!sq_desc.supports_integrated_auth);
    STATIC_REQUIRE(!sq_desc.supports_tls);
    STATIC_REQUIRE(!sq_desc.supports_root_cert);
    STATIC_REQUIRE(sq_desc.supports_in_memory);
    STATIC_REQUIRE(!sq_desc.file_filter.empty());

    // SQLite ConnectionProfile validation & store round-trip (port 0, empty host/username valid)
    TempDirGuard tmp("sqlite_profiles");
    const std::filesystem::path profiles_file = tmp.path() / "connections.json";

    ConnectionProfile sq_prof;
    sq_prof.id       = generate_uuid();
    sq_prof.name     = "LocalAppCache";
    sq_prof.engine   = "SQLite";
    sq_prof.host     = "";
    sq_prof.port     = 0;
    sq_prof.database = "C:/dados/app_cache.db";
    sq_prof.username = "";
    sq_prof.group    = "Local";
    sq_prof.disable_query_history = true;

    REQUIRE(sq_prof.validate().empty());
    REQUIRE(save_connection_profile(profiles_file, sq_prof));

    const auto loaded = load_connection_profiles(profiles_file);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].name == "LocalAppCache");
    REQUIRE(loaded[0].engine == "SQLite");
    REQUIRE(loaded[0].port == 0);
    REQUIRE(loaded[0].database == "C:/dados/app_cache.db");
    REQUIRE(loaded[0].group == std::optional<std::string>{"Local"});
    REQUIRE(is_query_history_disabled_for_connection(profiles_file, "SQLite", "localhost", 0, "C:/dados/app_cache.db"));
    REQUIRE_FALSE(is_query_history_disabled_for_connection(profiles_file, "SQLite", "localhost", 0, "C:/dados/other.db"));

    // parse_connection_uri tests
    const auto pg_uri = parse_connection_uri(
        "postgresql://alice:s3cr%40t@db.internal.local:5433/analytics?sslmode=require&connect_timeout=25&application_name=ReportTool");
    REQUIRE(pg_uri.has_value());
    REQUIRE(pg_uri->config.engine == EngineKind::PostgreSql);
    REQUIRE(pg_uri->config.host == "db.internal.local");
    REQUIRE(pg_uri->config.port == 5433);
    REQUIRE(pg_uri->config.database == "analytics");
    REQUIRE(pg_uri->config.username == "alice");
    REQUIRE(pg_uri->config.password == "s3cr@t");
    REQUIRE(pg_uri->config.tls_mode == TlsMode::Require);
    REQUIRE(pg_uri->config.connect_timeout_sec == 25);
    REQUIRE(pg_uri->config.application_name == "ReportTool");

    const auto sqlite_uri = parse_connection_uri("sqlite:///C:/dados/app.db");
    REQUIRE(sqlite_uri.has_value());
    REQUIRE(sqlite_uri->config.engine == EngineKind::Sqlite);
    REQUIRE(sqlite_uri->config.port == 0);
    REQUIRE(sqlite_uri->config.database == "C:/dados/app.db");

    const auto sqlite_file = parse_connection_uri("file:meu_banco.db");
    REQUIRE(sqlite_file.has_value());
    REQUIRE(sqlite_file->config.engine == EngineKind::Sqlite);
    REQUIRE(sqlite_file->config.database == "meu_banco.db");

    const auto mssql_kv = parse_connection_uri(
        "Server=erp-sql.corp,14330;Database=ERP_Prod;User Id=sa;Password=MyPass123;Encrypt=Strict;");
    REQUIRE(mssql_kv.has_value());
    REQUIRE(mssql_kv->config.engine == EngineKind::SqlServer);
    REQUIRE(mssql_kv->config.host == "erp-sql.corp");
    REQUIRE(mssql_kv->config.port == 14330);
    REQUIRE(mssql_kv->config.database == "ERP_Prod");
    REQUIRE(mssql_kv->config.username == "sa");
    REQUIRE(mssql_kv->config.password == "MyPass123");
    REQUIRE(mssql_kv->config.tls_mode == TlsMode::VerifyFull);

    REQUIRE_FALSE(parse_connection_uri("not a valid uri or connection string").has_value());
}

TEST_CASE("CancelToken invokes scoped callbacks and CatalogWorkerPool lazy-spawns threads on first task submission", "[core][threading]") {
    STATIC_REQUIRE(sizeof(capybaradb::core::CancelToken) <= 32);
    capybaradb::core::CancelToken token;
    int callback_hits = 0;
    {
        capybaradb::core::ScopedCancelCallback scope(token, [&]() { ++callback_hits; });
        token.cancel();
        REQUIRE(callback_hits == 1);
    }
    token.reset();
    REQUIRE_FALSE(token.is_canceled());
    token.cancel();
    REQUIRE(callback_hits == 1); // cleared cleanly when scope ended

    // Pre-canceled token invokes ScopedCancelCallback immediately on construction
    int pre_hits = 0;
    {
        capybaradb::core::ScopedCancelCallback pre_scope(token, [&]() { ++pre_hits; });
        REQUIRE(pre_hits == 1);
    }

    // Compact CatalogNodeData single-byte char tags
    auto rel = capybaradb::drivers::CatalogNodeData::make_folder(
        capybaradb::drivers::CatalogKind::Relation, "users", "appdb", "public", 100);
    rel.relation_type = 'r';
    REQUIRE(rel.relation_type == 'r');
    REQUIRE(rel.data_type_kind == '\0');
    REQUIRE(rel.routine_kind == '\0');
    REQUIRE(rel.can_expand());

    std::atomic<int> counter{0};
    {
        capybaradb::core::CatalogWorkerPool pool(2);
        REQUIRE(pool.worker_count() == 0); // Lazy-spawn: 0 threads before first submit()
        REQUIRE(pool.configured_worker_count() == 2);
        for (int i = 0; i < 8; ++i) {
            pool.submit([&]() { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        REQUIRE(pool.worker_count() == 2);
        pool.stop();
        REQUIRE(pool.worker_count() == 0);
    }
    REQUIRE(counter.load() == 8);
}

TEST_CASE("TSqlBatchSplitter splits GO batches with repeat counts and CatalogFilter matches name and owner criteria", "[drivers][catalog]") {
    const std::string script =
        "SELECT 'GO' AS literal_go;\n"
        "-- GO comment\n"
        "/* nested /* GO */ still comment */\n"
        "GO\n"
        "SELECT 1 AS [GO];\n"
        "GO 3 -- repeat 3 times\n";

    const auto batches = capybaradb::drivers::split_tsql_batches(script);
    REQUIRE(batches.size() == 2);
    REQUIRE(batches[0].repeat_count == 1);
    REQUIRE(batches[0].sql.find("literal_go") != std::string::npos);
    REQUIRE(batches[1].repeat_count == 3);
    REQUIRE(batches[1].sql.find("SELECT 1") != std::string::npos);

    capybaradb::drivers::CatalogFilter filter;
    filter.name = "ord";
    filter.name_operator = capybaradb::drivers::CatalogFilterOperator::Contains;
    filter.owner = "alice";
    filter.owner_operator = capybaradb::drivers::CatalogFilterOperator::Equals;

    REQUIRE(filter.is_active());
    REQUIRE(filter.matches("Orders", "ALICE"));
    REQUIRE_FALSE(filter.matches("Orders", "bob"));
    REQUIRE_FALSE(filter.matches("Users", "alice"));
}

namespace {

class MockCancellableDriver final : public capybaradb::drivers::DbDriver {
public:
    std::atomic<bool> cancel_called{false};
    int               cancel_delay_ms = 0;

    std::expected<void, capybaradb::drivers::DbError> connect(
        const capybaradb::drivers::ConnectionConfig&,
        capybaradb::core::CancelToken&) override {
        connected_ = true;
        return {};
    }

    void disconnect() noexcept override { connected_ = false; }
    bool is_connected() const noexcept override { return connected_; }
    capybaradb::drivers::SessionInfo session_info() const override { return {}; }

    std::expected<capybaradb::drivers::ConnectionTestResult, capybaradb::drivers::DbError> test_connection(
        const capybaradb::drivers::ConnectionConfig&,
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::ConnectionTestResult{};
    }

    std::expected<void, capybaradb::drivers::DbError> switch_database(
        std::string_view,
        capybaradb::core::CancelToken&) override {
        return {};
    }

    std::expected<capybaradb::drivers::SessionLossRisk, capybaradb::drivers::DbError> inspect_loss_risk(
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::SessionLossRisk{};
    }

    std::expected<void, capybaradb::drivers::DbError> commit_transaction(
        capybaradb::core::CancelToken&) override {
        return {};
    }

    std::expected<void, capybaradb::drivers::DbError> execute_stream(
        std::string_view,
        const capybaradb::drivers::ExecOptions&,
        capybaradb::core::QueryExecutionResult&,
        capybaradb::core::StreamPublishSink&,
        capybaradb::core::CancelToken& cancel) override {
        std::unique_lock<std::mutex> lock(mu_);
        running_.store(true, std::memory_order_release);
        started_cv_.notify_all();
        cancel_cv_.wait(lock, [&]() { return cancel.is_canceled() || cancel_called.load(std::memory_order_acquire); });
        return std::unexpected(capybaradb::drivers::DbError{.message = "Canceled", .is_canceled = true});
    }

    std::expected<void, capybaradb::drivers::DbError> parse_only(
        std::string_view,
        capybaradb::core::CancelToken&) override {
        return {};
    }

    void cancel_active() noexcept override {
        if (cancel_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cancel_delay_ms));
        }
        cancel_called.store(true, std::memory_order_release);
        cancel_cv_.notify_all();
    }

    void wait_until_running() {
        std::unique_lock<std::mutex> lock(mu_);
        started_cv_.wait(lock, [&]() { return running_.load(std::memory_order_acquire); });
    }

private:
    bool                    connected_ = false;
    std::atomic<bool>       running_{false};
    std::mutex              mu_;
    std::condition_variable started_cv_;
    std::condition_variable cancel_cv_;
};

} // namespace

TEST_CASE("PostgresDialect incremental scanner tokenizes nested comments, E-strings, dollar-quotes, and keywords", "[dialects][postgres]") {
    using namespace capybaradb::dialects;
    const auto eng = capybaradb::drivers::EngineKind::PostgreSql;
    STATIC_REQUIRE(std::is_trivially_copyable_v<LexicalState> && sizeof(LexicalState) == 64);
    REQUIRE(is_sql_keyword(eng, "select"));
    REQUIRE(is_sql_keyword(eng, "RETURNING"));
    REQUIRE(is_sql_keyword(eng, "MATERIALIZED"));
    REQUIRE_FALSE(is_sql_keyword(eng, "nonexistent_kw"));

    std::vector<TokenSpan> tokens;
    LexicalState s1 = scan_sql_line(eng, "/* outer /* inner", LexicalState{}, &tokens);
    REQUIRE(s1.kind == LexicalKind::Comment);
    REQUIRE(s1.comment_depth == 2);
    REQUIRE(s1.to_block_state_int() != 0);

    tokens.clear();
    LexicalState s2 = scan_sql_line(eng, "still comment */ */ SELECT $tag$hello", s1, &tokens);
    REQUIRE(s2.kind == LexicalKind::DollarString);
    REQUIRE(s2.dollar_delimiter() == "$tag$");

    tokens.clear();
    LexicalState s3 = scan_sql_line(eng, "world$tag$ 'it''s' E'a\\'b' \"MyCol\" -- end", s2, &tokens);
    REQUIRE(s3.kind == LexicalKind::Code);
    REQUIRE(s3.comment_depth == 0);
    REQUIRE(s3.dollar_delimiter().empty());
    REQUIRE(s3.is_default_code());

    auto has_kind = [](const std::vector<TokenSpan>& spans, LexicalKind k) {
        return std::any_of(spans.begin(), spans.end(), [k](const TokenSpan& sp) { return sp.kind == k; });
    };
    REQUIRE(has_kind(tokens, LexicalKind::DollarString));
    REQUIRE(has_kind(tokens, LexicalKind::String));
    REQUIRE(has_kind(tokens, LexicalKind::EscapeString));
    REQUIRE(has_kind(tokens, LexicalKind::QuotedIdentifier));
    REQUIRE(has_kind(tokens, LexicalKind::Comment));
}

TEST_CASE("TSqlDialect incremental scanner tokenizes GO, bracket identifiers, variables, and nested comments", "[dialects][tsql]") {
    using namespace capybaradb::dialects;
    const auto eng = capybaradb::drivers::EngineKind::SqlServer;
    REQUIRE(is_sql_keyword(eng, "GO"));
    REQUIRE(is_sql_keyword(eng, "nvarchar"));

    std::vector<TokenSpan> tokens;
    LexicalState st = scan_sql_line(eng, "SELECT @v, @@ROWCOUNT, [My Col], N'o''k' -- note", LexicalState{}, &tokens);
    REQUIRE(st.kind == LexicalKind::Code);

    auto has_kind = [&tokens](LexicalKind k) {
        return std::any_of(tokens.begin(), tokens.end(), [k](const TokenSpan& sp) { return sp.kind == k; });
    };
    REQUIRE(has_kind(LexicalKind::Keyword));
    REQUIRE(has_kind(LexicalKind::LocalVariable));
    REQUIRE(has_kind(LexicalKind::GlobalVariable));
    REQUIRE(has_kind(LexicalKind::BracketIdentifier));
    REQUIRE(has_kind(LexicalKind::String));
    REQUIRE(has_kind(LexicalKind::Comment));
}

TEST_CASE("SqlPlaceholderScanner detects unbound PostgreSQL positional parameters and exempts PREPARE and function bodies", "[dialects][postgres]") {
    using namespace capybaradb::dialects;
    const auto found = find_postgres_unbound(
        "SELECT 'a$1b', $q$x$2y$q$, col FROM t WHERE id = $2 AND v = $1");
    REQUIRE(found.has_value());
    REQUIRE(found->first_number == 2);
    REQUIRE(found->highest_number == 2);
    REQUIRE(found->count == 2);
    REQUIRE_FALSE(found->description().empty());

    // Multi-byte UTF-8 prefix before $1: byte_offset > UTF-16 char_offset
    const std::string utf8_sql = "SELECT '\xC3\xA1\xC3\xA9', $1";
    const auto non_ascii = find_postgres_unbound(utf8_sql);
    REQUIRE(non_ascii.has_value());
    REQUIRE(non_ascii->byte_offset > non_ascii->char_offset);

    REQUIRE_FALSE(find_postgres_unbound("PREPARE p(int) AS SELECT $1;").has_value());
    REQUIRE_FALSE(find_postgres_unbound(
        "CREATE OR REPLACE FUNCTION f(int) RETURNS int AS $$ SELECT $1 $$ LANGUAGE sql;").has_value());
}

TEST_CASE("BoundedNoticeLog caps messages, ResultFormatter formats text results, and DbSessionWorker cancels non-blockingly", "[core][threading][stream]") {
    capybaradb::core::BoundedNoticeLog log;
    for (int i = 0; i < 105; ++i) {
        log.add("NOTICE", "00000", "msg " + std::to_string(i));
    }
    REQUIRE(log.snapshot().size() == 100);
    REQUIRE(log.omitted_count() == 5);
    log.add("NOTICE", "00000", std::string(5000, 'x'));
    REQUIRE(log.omitted_count() == 6);

    capybaradb::core::QueryExecutionResult qres;
    capybaradb::core::StreamPublishSink sink;
    const std::array<capybaradb::core::ColumnSpec, 2> cols = {{
        {"id", "int4", capybaradb::core::LogicalTypeKind::Int, false},
        {"name", "text", capybaradb::core::LogicalTypeKind::Text, true},
    }};
    capybaradb::core::ResultSetStorage* rs = qres.create_result_set(cols);
    REQUIRE(rs != nullptr);
    capybaradb::drivers::ExecOptions opts;
    {
        capybaradb::core::ResultSetWriter writer(qres, *rs, opts.max_arena_bytes, opts.max_cell_bytes);
        REQUIRE(writer.begin_row());
        writer.write_utf8(0, "1", 1);
        writer.write_utf8(1, "Alice", 5);
        (void)writer.end_row(false);

        REQUIRE(writer.begin_row());
        writer.write_utf8(0, "2", 1);
        writer.write_null(1);
        (void)writer.end_row(false);
        writer.finish();
    }
    qres.add_rows_affected(2, rs);
    qres.append_message(capybaradb::core::MessageSeverity::Notice, "00000", "hello notice");
    const std::string text = capybaradb::core::format_results_as_text(qres, 0.125, true, false, std::nullopt);
    REQUIRE(text.find("Alice") != std::string::npos);
    REQUIRE(text.find("NULL") != std::string::npos);
    REQUIRE(text.find("(2 row(s) returned)") != std::string::npos);
    REQUIRE(text.find("Elapsed: 0.125 s.") != std::string::npos);
    REQUIRE(text.find("Completion time: ") != std::string::npos);

    capybaradb::core::DbSessionWorker worker;
    auto mock_drv = std::make_unique<MockCancellableDriver>();
    MockCancellableDriver* raw_drv = mock_drv.get();
    worker.replace_driver(std::move(mock_drv));

    std::atomic<bool> task_done{false};
    auto tok = worker.submit([&](capybaradb::core::DbSessionWorker& w, capybaradb::core::CancelToken& c) {
        (void)w.driver()->execute_stream("WAIT", opts, qres, sink, c);
        task_done.store(true, std::memory_order_release);
    });
    raw_drv->cancel_delay_ms = 120;
    raw_drv->wait_until_running();
    const auto t0 = std::chrono::steady_clock::now();
    worker.cancel_active();
    const auto call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(call_ms < 50); // Must return immediately without blocking the UI thread
    while (!task_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(tok->is_canceled());
    REQUIRE(ms < 300);

    // Verify 64-slot ring buffer wrap-around across 3 batches of 50 tasks (150 total > 64)
    std::atomic<int> ring_sum{0};
    for (int batch = 0; batch < 3; ++batch) {
        std::atomic<int> batch_done{0};
        for (int j = 1; j <= 50; ++j) {
            const int i = batch * 50 + j;
            auto* slot_tok = worker.submit([i, &ring_sum, &batch_done](capybaradb::core::DbSessionWorker&, capybaradb::core::CancelToken& c) {
                if (!c.is_canceled()) {
                    ring_sum.fetch_add(i, std::memory_order_relaxed);
                }
                batch_done.fetch_add(1, std::memory_order_release);
            });
            REQUIRE(slot_tok != nullptr);
            REQUIRE_FALSE(slot_tok->is_canceled());
        }
        while (batch_done.load(std::memory_order_acquire) < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    worker.stop();
    REQUIRE(raw_drv->cancel_called.load());
    REQUIRE(ring_sum.load() == (150 * 151) / 2);
}
