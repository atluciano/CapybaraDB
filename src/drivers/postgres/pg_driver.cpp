#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include "drivers/postgres/pg_driver.hpp"

#include "dialects/dialect_lex_utils.hpp"
#include "drivers/postgres/pg_type_oids.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace capybaradb::drivers {
namespace {

const char* to_libpq_sslmode(TlsMode mode) noexcept {
    switch (mode) {
    case TlsMode::VerifyFull: return "verify-full";
    case TlsMode::Require:    return "require";
    case TlsMode::Prefer:     return "prefer";
    case TlsMode::Disable:    return "disable";
    }
    return "verify-full";
}

bool wait_socket(int sock, bool wait_read, bool wait_write, int timeout_ms) noexcept {
    if (sock < 0) {
        return false;
    }
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = static_cast<SOCKET>(sock);
    pfd.events = static_cast<SHORT>((wait_read ? POLLRDNORM : 0) | (wait_write ? POLLWRNORM : 0));
    const int rc = WSAPoll(&pfd, 1, timeout_ms);
    return rc > 0;
#else
    struct pollfd pfd{};
    pfd.fd = sock;
    pfd.events = static_cast<short>((wait_read ? POLLIN : 0) | (wait_write ? POLLOUT : 0));
    const int rc = ::poll(&pfd, 1, timeout_ms);
    return rc > 0;
#endif
}

std::string diag_str(const PGresult* res, int field_code) {
    if (res == nullptr) {
        return {};
    }
    const char* v = PQresultErrorField(res, field_code);
    return v != nullptr ? std::string{v} : std::string{};
}

std::string trim_trailing_newlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

class ActiveCancelGuard final {
public:
    ActiveCancelGuard(PGconn* conn, core::CancelToken& token)
        : token_(token)
        , cancel_obj_(PQgetCancel(conn)) {
        token_.set_on_cancel(&ActiveCancelGuard::on_cancel_trampoline, this);
    }

    ~ActiveCancelGuard() noexcept {
        token_.clear_on_cancel();
        std::lock_guard<std::mutex> lock(mu_);
        if (cancel_obj_ != nullptr) {
            PQfreeCancel(cancel_obj_);
            cancel_obj_ = nullptr;
        }
    }

    void fire_cancel() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancel_obj_ != nullptr) {
            char errbuf[256]{};
            (void)PQcancel(cancel_obj_, errbuf, static_cast<int>(sizeof(errbuf)));
        }
    }

private:
    static void on_cancel_trampoline(void* ctx) noexcept {
        static_cast<ActiveCancelGuard*>(ctx)->fire_cancel();
    }

    core::CancelToken& token_;
    std::mutex         mu_;
    pg_cancel*         cancel_obj_ = nullptr;
};

struct PgNoticeContext {
    core::QueryExecutionResult* out                  = nullptr;
    core::StreamPublishSink*    sink                 = nullptr;
    bool                        capture_actual_plans = false;
    std::string*                arm_failure_out      = nullptr;
};

constexpr std::string_view kPgPlanCaptureFailureMarker = "capybaradb-plan-capture:";
constexpr std::string_view kPgPlanMessageMarker        = " ms  plan:\n";

void pg_notice_receiver(void* arg, const PGresult* res) {
    auto* ctx = static_cast<PgNoticeContext*>(arg);
    if (ctx == nullptr || res == nullptr) {
        return;
    }
    std::string severity = diag_str(res, PG_DIAG_SEVERITY_NONLOCALIZED);
    if (severity.empty()) {
        severity = diag_str(res, PG_DIAG_SEVERITY);
    }
    std::string msg = trim_trailing_newlines(diag_str(res, PG_DIAG_MESSAGE_PRIMARY));
    if (msg.empty()) {
        if (const char* raw = PQresultErrorMessage(res)) {
            msg = trim_trailing_newlines(raw);
        }
    }
    if (msg.empty()) {
        return;
    }

    if (ctx->arm_failure_out != nullptr && msg.starts_with(kPgPlanCaptureFailureMarker)) {
        *ctx->arm_failure_out = msg.substr(kPgPlanCaptureFailureMarker.size());
        return;
    }

    if (ctx->capture_actual_plans && severity == "INFO" && msg.starts_with("duration: ")) {
        const size_t pos = msg.find(kPgPlanMessageMarker);
        if (pos != std::string::npos) {
            if (ctx->out != nullptr) {
                const std::string json_obj = msg.substr(pos + kPgPlanMessageMarker.size());
                if (json_obj.size() + 2u <= core::kMaxExecutionPlanBytes) {
                    ctx->out->add_actual_plan(core::RawExecutionPlan{
                        .source_kind = core::PlanSourceKind::PostgreSqlJson,
                        .raw_text    = "[" + json_obj + "]",
                        .analyzed    = true,
                        .query_text  = {},
                    });
                }
            }
            return; // Never pollute Messages with auto_explain JSON plan payloads
        }
    }

    if (ctx->out == nullptr) {
        return;
    }
    const core::MessageSeverity msg_sev = (severity == "WARNING")
        ? core::MessageSeverity::Warning
        : (severity == "INFO") ? core::MessageSeverity::Info
                               : core::MessageSeverity::Notice;
    std::string sql_state = diag_str(res, PG_DIAG_SQLSTATE);
    ctx->out->append_message(msg_sev, sql_state, msg);
    if (ctx->sink != nullptr) {
        ctx->sink->notify_if_unscheduled();
    }
}

void noop_notice_receiver(void*, const PGresult*) {}

struct PgStatementPiece {
    std::string sql;
    int32_t     char_offset = 0; // 0-based UTF-8/char prefix count for position adjustment
};

std::vector<PgStatementPiece> split_pg_statements(std::string_view sql) {
    std::vector<PgStatementPiece> out;
    size_t stmt_start = 0;
    size_t i = 0;

    auto flush_piece = [&](size_t end) {
        size_t s = stmt_start;
        while (s < end && std::isspace(static_cast<unsigned char>(sql[s]))) {
            ++s;
        }
        size_t e = end;
        while (e > s && std::isspace(static_cast<unsigned char>(sql[e - 1]))) {
            --e;
        }
        if (e > s) {
            int32_t chars_before = 0;
            for (size_t k = 0; k < s; ++k) {
                if ((static_cast<unsigned char>(sql[k]) & 0xC0u) != 0x80u) {
                    ++chars_before;
                }
            }
            out.push_back(PgStatementPiece{std::string(sql.substr(s, e - s)), chars_before});
        }
        stmt_start = end + 1;
    };

    while (i < sql.size()) {
        const char c = sql[i];
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int depth = 1;
            i += 2;
            while (i < sql.size() && depth > 0) {
                if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                    ++depth;
                    i += 2;
                } else if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            bool closed = false;
            ++i;
            dialects::detail::advance_quoted(sql, i, c, false, closed);
            continue;
        }
        if ((c == 'E' || c == 'e') && i + 1 < sql.size() && sql[i + 1] == '\'' &&
            (i == 0 || !dialects::detail::is_sql_ident_part(sql[i - 1], true))) {
            bool closed = false;
            i += 2;
            dialects::detail::advance_quoted(sql, i, '\'', true, closed);
            continue;
        }
        if (c == '$') {
            size_t tag_end = i + 1;
            if (tag_end < sql.size() && sql[tag_end] != '$' && dialects::detail::is_sql_ident_start(sql[tag_end])) {
                while (tag_end < sql.size() && dialects::detail::is_sql_ident_part(sql[tag_end], false)) {
                    ++tag_end;
                }
            }
            if (tag_end < sql.size() && sql[tag_end] == '$') {
                const std::string_view tag = sql.substr(i, tag_end - i + 1);
                const size_t close = sql.find(tag, tag_end + 1);
                i = (close == std::string_view::npos) ? sql.size() : (close + tag.size());
                continue;
            }
        }
        if (c == ';') {
            flush_piece(i);
            ++i;
            continue;
        }
        ++i;
    }
    flush_piece(sql.size());
    return out;
}

} // namespace

void PgConnDeleter::operator()(pg_conn* conn) const noexcept {
    if (conn != nullptr) {
        PQfinish(conn);
    }
}

void PgResultDeleter::operator()(pg_result* res) const noexcept {
    if (res != nullptr) {
        PQclear(res);
    }
}

PgDriver::~PgDriver() {
    disconnect();
}

void PgDriver::disconnect() noexcept {
    cancel_active();
    std::lock_guard<std::mutex> lock(conn_mu_);
    conn_.reset();
    session_info_ = {};
    custom_oid_names_.clear();
    plan_capture_armed_ = false;
}

bool PgDriver::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(conn_mu_);
    return conn_ != nullptr && PQstatus(conn_.get()) == CONNECTION_OK;
}

SessionInfo PgDriver::session_info() const {
    std::lock_guard<std::mutex> lock(conn_mu_);
    return session_info_;
}

void PgDriver::cancel_active() noexcept {
    std::lock_guard<std::mutex> lock(cancel_mu_);
    if (active_cancel_ != nullptr) {
        char errbuf[256]{};
        (void)PQcancel(active_cancel_, errbuf, static_cast<int>(sizeof(errbuf)));
    }
}

DbError PgDriver::error_from_result(const pg_result* res, const pg_conn* conn) {
    DbError err;
    if (res != nullptr) {
        err.sql_state  = diag_str(res, PG_DIAG_SQLSTATE);
        err.severity   = diag_str(res, PG_DIAG_SEVERITY_NONLOCALIZED);
        if (err.severity.empty()) {
            err.severity = diag_str(res, PG_DIAG_SEVERITY);
        }
        err.message    = trim_trailing_newlines(diag_str(res, PG_DIAG_MESSAGE_PRIMARY));
        err.hint       = diag_str(res, PG_DIAG_MESSAGE_HINT);
        err.schema     = diag_str(res, PG_DIAG_SCHEMA_NAME);
        err.table      = diag_str(res, PG_DIAG_TABLE_NAME);
        err.column     = diag_str(res, PG_DIAG_COLUMN_NAME);
        err.data_type  = diag_str(res, PG_DIAG_DATATYPE_NAME);
        err.constraint = diag_str(res, PG_DIAG_CONSTRAINT_NAME);
        if (const std::string pos = diag_str(res, PG_DIAG_STATEMENT_POSITION); !pos.empty()) {
            err.position = std::atoi(pos.c_str());
        }
        if (err.message.empty()) {
            const char* raw = PQresultErrorMessage(res);
            if (raw != nullptr) {
                err.message = trim_trailing_newlines(raw);
            }
        }
        if (err.sql_state == "57014") {
            err.is_canceled = true;
        }
    }
    if (err.message.empty() && conn != nullptr) {
        const char* raw = PQerrorMessage(conn);
        if (raw != nullptr) {
            err.message = trim_trailing_newlines(raw);
        }
    }
    if (err.message.empty()) {
        err.message = "Unknown PostgreSQL error.";
    }
    return err;
}

std::expected<PgConnPtr, DbError> PgDriver::open_connection(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Connection canceled.", .is_canceled = true});
    }

    const std::string port_str    = std::to_string(cfg.port);
    const std::string timeout_str = std::to_string(cfg.connect_timeout_sec > 0 ? cfg.connect_timeout_sec : 10);
    const char* sslmode = to_libpq_sslmode(cfg.tls_mode);

    std::vector<const char*> keys;
    std::vector<const char*> vals;
    auto push_opt = [&](const char* k, const char* v) {
        if (v != nullptr && *v != '\0') {
            keys.push_back(k);
            vals.push_back(v);
        }
    };

    push_opt("host", cfg.host.c_str());
    push_opt("port", port_str.c_str());
    push_opt("dbname", cfg.database.c_str());
    push_opt("user", cfg.username.c_str());
    if (!cfg.password.empty()) {
        keys.push_back("password");
        vals.push_back(cfg.password.c_str());
    }
    push_opt("application_name", cfg.application_name.empty() ? "CapybaraDB" : cfg.application_name.c_str());
    push_opt("connect_timeout", timeout_str.c_str());
    push_opt("sslmode", sslmode);
    if (!cfg.root_cert_path.empty()) {
        push_opt("sslrootcert", cfg.root_cert_path.c_str());
    }
    push_opt("client_encoding", "UTF8");
    keys.push_back(nullptr);
    vals.push_back(nullptr);

    PgConnPtr conn(PQconnectStartParams(keys.data(), vals.data(), 0));
    if (!conn) {
        return std::unexpected(DbError{.message = "Out of memory allocating PostgreSQL connection."});
    }
    if (PQstatus(conn.get()) == CONNECTION_BAD) {
        return std::unexpected(error_from_result(nullptr, conn.get()));
    }

    const auto timeout_dur = std::chrono::seconds((cfg.connect_timeout_sec > 0 ? cfg.connect_timeout_sec : 10) + 2);
    const auto start = std::chrono::steady_clock::now();

    PostgresPollingStatusType poll_status = PGRES_POLLING_WRITING;
    for (;;) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Connection canceled.", .is_canceled = true});
        }
        if (std::chrono::steady_clock::now() - start > timeout_dur) {
            return std::unexpected(DbError{.message = "Connection timed out."});
        }

        poll_status = PQconnectPoll(conn.get());
        if (poll_status == PGRES_POLLING_OK) {
            break;
        }
        if (poll_status == PGRES_POLLING_FAILED) {
            return std::unexpected(error_from_result(nullptr, conn.get()));
        }

        const int sock = PQsocket(conn.get());
        if (sock < 0) {
            return std::unexpected(error_from_result(nullptr, conn.get()));
        }
        const bool want_r = (poll_status == PGRES_POLLING_READING);
        const bool want_w = (poll_status == PGRES_POLLING_WRITING);
        (void)wait_socket(sock, want_r, want_w, 50);
    }

    return conn;
}

std::expected<PgResultPtr, DbError> PgDriver::exec_params_cancelable(
    pg_conn* conn,
    const char* sql,
    std::span<const char* const> param_values,
    core::CancelToken& cancel,
    uint32_t timeout_sec) {
    if (conn == nullptr) {
        return std::unexpected(DbError{.message = "Not connected to PostgreSQL."});
    }
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }

    const int n_params = static_cast<int>(param_values.size());
    const int sent = PQsendQueryParams(
        conn,
        sql,
        n_params,
        nullptr,
        n_params > 0 ? param_values.data() : nullptr,
        nullptr,
        nullptr,
        0);
    if (sent == 0) {
        return std::unexpected(error_from_result(nullptr, conn));
    }

    ActiveCancelGuard guard(conn, cancel);
    const auto start = std::chrono::steady_clock::now();
    const auto limit = std::chrono::seconds(timeout_sec > 0 ? timeout_sec : 30);

    for (;;) {
        if (cancel.is_canceled()) {
            guard.fire_cancel();
        }
        if (timeout_sec > 0 && std::chrono::steady_clock::now() - start > limit) {
            guard.fire_cancel();
            return std::unexpected(DbError{.message = "PostgreSQL operation timed out."});
        }
        if (PQconsumeInput(conn) == 0) {
            return std::unexpected(error_from_result(nullptr, conn));
        }
        if (PQisBusy(conn) == 0) {
            break;
        }
        const int sock = PQsocket(conn);
        (void)wait_socket(sock, true, false, 50);
    }

    PgResultPtr first_res;
    while (PGresult* raw = PQgetResult(conn)) {
        PgResultPtr cur(raw);
        if (!first_res) {
            first_res = std::move(cur);
        }
    }

    if (!first_res) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        return std::unexpected(error_from_result(nullptr, conn));
    }

    const ExecStatusType st = PQresultStatus(first_res.get());
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK && st != PGRES_EMPTY_QUERY) {
        DbError err = error_from_result(first_res.get(), conn);
        if (cancel.is_canceled()) {
            err.is_canceled = true;
        }
        return std::unexpected(std::move(err));
    }

    return first_res;
}

std::expected<SessionInfo, DbError> PgDriver::query_session_info(
    pg_conn* conn,
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    static constexpr const char* kSql =
        "SELECT current_database(), current_user, version(), pg_backend_pid()";
    auto res = exec_params_cancelable(conn, kSql, {}, cancel, 10);
    if (!res) {
        return std::unexpected(res.error());
    }
    SessionInfo info;
    info.engine = EngineKind::PostgreSql;
    info.host   = cfg.host;
    info.port   = cfg.port;
    if (PQntuples(res->get()) > 0) {
        info.database       = PQgetvalue(res->get(), 0, 0);
        info.username       = PQgetvalue(res->get(), 0, 1);
        info.server_version = PQgetvalue(res->get(), 0, 2);
        info.session_id     = std::strtoll(PQgetvalue(res->get(), 0, 3), nullptr, 10);
    }
    return info;
}

std::unordered_map<uint32_t, std::string> PgDriver::load_extension_type_map(
    pg_conn* conn,
    core::CancelToken& cancel) {
    std::unordered_map<uint32_t, std::string> map;
    if (conn == nullptr) return map;
    static constexpr const char* kSql =
        "SELECT oid, typname FROM pg_catalog.pg_type "
        "WHERE oid >= 16384 AND typtype IN ('b','e','d','r','m') "
        "ORDER BY oid LIMIT 1024";
    auto res = exec_params_cancelable(conn, kSql, {}, cancel, 5);
    if (!res) return map;
    const int n = PQntuples(res->get());
    map.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* oid_s = PQgetvalue(res->get(), i, 0);
        const char* name_s = PQgetvalue(res->get(), i, 1);
        if (oid_s != nullptr && name_s != nullptr) {
            const uint32_t oid = static_cast<uint32_t>(std::strtoul(oid_s, nullptr, 10));
            if (oid > 0) {
                map.emplace(oid, std::string{name_s});
            }
        }
    }
    return map;
}

std::expected<void, DbError> PgDriver::connect(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    auto opened = open_connection(cfg, cancel);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto info = query_session_info(opened->get(), cfg, cancel);
    if (!info) {
        return std::unexpected(info.error());
    }
    auto ext_types = load_extension_type_map(opened->get(), cancel);

    std::lock_guard<std::mutex> lock(conn_mu_);
    conn_               = std::move(*opened);
    current_cfg_        = cfg;
    session_info_       = std::move(*info);
    custom_oid_names_   = std::move(ext_types);
    plan_capture_armed_ = false;
    return {};
}

std::expected<ConnectionTestResult, DbError> PgDriver::test_connection(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    auto opened = open_connection(cfg, cancel);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    static constexpr const char* kSql =
        "SELECT current_database(), current_user, version(), "
        "COALESCE((SELECT ssl FROM pg_catalog.pg_stat_ssl WHERE pid=pg_backend_pid()), false)";
    auto res = exec_params_cancelable(opened->get(), kSql, {}, cancel, 10);
    if (!res) {
        return std::unexpected(res.error());
    }
    if (PQntuples(res->get()) < 1) {
        return std::unexpected(DbError{.message = "The server connection opened but did not return connection details."});
    }

    ConnectionTestResult out;
    out.database   = PQgetvalue(res->get(), 0, 0);
    out.user       = PQgetvalue(res->get(), 0, 1);
    out.version    = PQgetvalue(res->get(), 0, 2);
    const char* tls_txt = PQgetvalue(res->get(), 0, 3);
    out.tls_active = (tls_txt != nullptr && (tls_txt[0] == 't' || tls_txt[0] == '1'));
    return out;
}

std::expected<void, DbError> PgDriver::switch_database(
    std::string_view database,
    core::CancelToken& cancel) {
    ConnectionConfig next_cfg;
    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        if (!conn_) {
            return std::unexpected(DbError{.message = "Not connected."});
        }
        next_cfg = current_cfg_.with_database(std::string(database));
    }

    auto opened = open_connection(next_cfg, cancel);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto info = query_session_info(opened->get(), next_cfg, cancel);
    if (!info) {
        return std::unexpected(info.error());
    }
    auto ext_types = load_extension_type_map(opened->get(), cancel);

    std::lock_guard<std::mutex> lock(conn_mu_);
    conn_               = std::move(*opened);
    current_cfg_        = std::move(next_cfg);
    session_info_       = std::move(*info);
    custom_oid_names_   = std::move(ext_types);
    plan_capture_armed_ = false;
    return {};
}

std::expected<SessionLossRisk, DbError> PgDriver::inspect_loss_risk(
    core::CancelToken& cancel) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_) {
        return SessionLossRisk{};
    }

    const PGTransactionStatusType tx = PQtransactionStatus(conn_.get());
    SessionLossRisk risk;
    risk.aborted_transaction = (tx == PQTRANS_INERROR);
    risk.open_transaction    = (tx == PQTRANS_INTRANS || tx == PQTRANS_INERROR);

    if (!risk.aborted_transaction) {
        static constexpr const char* kTempSql =
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_class "
            "WHERE relnamespace = pg_my_temp_schema() AND relkind IN ('r','p','v','m','S'))";
        auto res = exec_params_cancelable(conn_.get(), kTempSql, {}, cancel, 5);
        if (res && PQntuples(res->get()) > 0) {
            const char* v = PQgetvalue(res->get(), 0, 0);
            risk.temporary_tables = (v != nullptr && (v[0] == 't' || v[0] == '1'));
        }
    }
    return risk;
}

std::expected<void, DbError> PgDriver::commit_transaction(
    core::CancelToken& cancel) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_) {
        return std::unexpected(DbError{.message = "Not connected."});
    }
    auto res = exec_params_cancelable(conn_.get(), "COMMIT", {}, cancel, 10);
    if (!res) {
        return std::unexpected(res.error());
    }
    return {};
}

std::expected<void, DbError> PgDriver::execute_stream(
    std::string_view sql,
    const ExecOptions& opts,
    core::QueryExecutionResult& out,
    core::StreamPublishSink& sink,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        out.canceled.store(true, std::memory_order_release);
        return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
    }
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) {
        return std::unexpected(DbError{.message = "Not connected to PostgreSQL."});
    }

    PGconn* raw_conn = conn_.get();

    // Register driver-wide active_cancel_
    {
        std::lock_guard<std::mutex> cl(cancel_mu_);
        if (active_cancel_ != nullptr) {
            PQfreeCancel(active_cancel_);
        }
        active_cancel_ = PQgetCancel(raw_conn);
    }
    struct CancelCleanup final {
        PgDriver* driver;
        ~CancelCleanup() noexcept {
            std::lock_guard<std::mutex> cl(driver->cancel_mu_);
            if (driver->active_cancel_ != nullptr) {
                PQfreeCancel(driver->active_cancel_);
                driver->active_cancel_ = nullptr;
            }
        }
    } cancel_cleanup{this};

    // Apply session isolation / lock_timeout when outside an open/aborted transaction
    const PGTransactionStatusType tx_before = PQtransactionStatus(raw_conn);
    if (tx_before == PQTRANS_IDLE) {
        if (opts.isolation != core::QueryIsolationLevel::ServerDefault) {
            const char* iso_sql = nullptr;
            switch (opts.isolation) {
            case core::QueryIsolationLevel::ReadCommitted:  iso_sql = "SET default_transaction_isolation = 'read committed'"; break;
            case core::QueryIsolationLevel::RepeatableRead: iso_sql = "SET default_transaction_isolation = 'repeatable read'"; break;
            case core::QueryIsolationLevel::Serializable:   iso_sql = "SET default_transaction_isolation = 'serializable'"; break;
            default: break;
            }
            if (iso_sql != nullptr) {
                (void)exec_params_cancelable(raw_conn, iso_sql, {}, cancel, 10);
            }
        }
        if (opts.lock_timeout_ms >= 0) {
            const std::string lt_sql = "SET lock_timeout = " + std::to_string(opts.lock_timeout_ms);
            (void)exec_params_cancelable(raw_conn, lt_sql.c_str(), {}, cancel, 10);
        }
    }

    // Arm or disarm auto_explain for Actual Execution Plan capture
    bool armed_this_exec = false;
    if (opts.include_actual_plan) {
        std::string arm_failure;
        PgNoticeContext arm_ctx{nullptr, nullptr, false, &arm_failure};
        PQsetNoticeReceiver(raw_conn, &pg_notice_receiver, &arm_ctx);
        static constexpr const char* kArmSql =
            "DO $capybaradb$ BEGIN "
            "EXECUTE 'LOAD ''auto_explain'''; "
            "PERFORM pg_catalog.set_config(s.name, s.value, false) FROM (VALUES "
            "('auto_explain.log_level','info'),('auto_explain.log_format','json'),"
            "('auto_explain.log_analyze','on'),('auto_explain.log_buffers','on'),"
            "('auto_explain.log_timing','on'),('auto_explain.log_verbose','on'),"
            "('auto_explain.log_nested_statements','off'),('auto_explain.sample_rate','1'),"
            "('auto_explain.log_min_duration','0')) AS s(name, value); "
            "EXCEPTION WHEN OTHERS THEN "
            "RAISE INFO USING MESSAGE = 'capybaradb-plan-capture:' || SQLSTATE || ': ' || SQLERRM; "
            "END $capybaradb$";
        auto arm_res = exec_params_cancelable(raw_conn, kArmSql, {}, cancel, 15);
        PQsetNoticeReceiver(raw_conn, &noop_notice_receiver, nullptr);
        if (!arm_res) {
            out.set_actual_plan_unavailable(arm_res.error().format_short());
        } else if (!arm_failure.empty()) {
            out.set_actual_plan_unavailable(arm_failure);
        } else {
            plan_capture_armed_ = true;
            armed_this_exec     = true;
        }
    } else if (plan_capture_armed_) {
        core::CancelToken disarm_tok;
        (void)exec_params_cancelable(raw_conn, "SELECT pg_catalog.set_config('auto_explain.log_min_duration','-1',false)", {}, disarm_tok, 10);
        plan_capture_armed_ = false;
    }

    struct PlanDisarmCleanup final {
        PgDriver* driver = nullptr;
        PGconn*   conn   = nullptr;
        bool      active = false;
        ~PlanDisarmCleanup() noexcept {
            if (active && conn != nullptr && PQstatus(conn) == CONNECTION_OK &&
                PQtransactionStatus(conn) != PQTRANS_INERROR) {
                core::CancelToken cleanup_tok;
                (void)PgDriver::exec_params_cancelable(
                    conn, "SELECT pg_catalog.set_config('auto_explain.log_min_duration','-1',false)", {}, cleanup_tok, 10);
                driver->plan_capture_armed_ = false;
            }
        }
    } plan_disarm{this, raw_conn, armed_this_exec};

    PgNoticeContext notice_ctx{&out, &sink, armed_this_exec, nullptr};
    PQsetNoticeReceiver(raw_conn, &pg_notice_receiver, &notice_ctx);
    struct NoticeCleanup final {
        PGconn* conn;
        ~NoticeCleanup() noexcept {
            PQsetNoticeReceiver(conn, &noop_notice_receiver, nullptr);
        }
    } notice_cleanup{raw_conn};

    const std::string sql_str(sql);
    if (PQsendQuery(raw_conn, sql_str.c_str()) == 0) {
        return std::unexpected(error_from_result(nullptr, raw_conn));
    }
    (void)PQsetSingleRowMode(raw_conn);

    const auto start_tp = std::chrono::steady_clock::now();
    auto last_flush_tp  = start_tp;
    const bool has_timeout = (opts.command_timeout_sec > 0);
    const auto timeout_dur = std::chrono::seconds(opts.command_timeout_sec);
    bool timeout_fired = false;
    bool cancel_sent   = false;
    auto send_cancel_once = [&]() {
        if (!cancel_sent) {
            cancel_sent = true;
            cancel_active();
        }
    };

    core::ResultSetStorage* current_rs = nullptr;
    std::unique_ptr<core::ResultSetWriter> writer;
    std::vector<bool> col_is_binary;
    std::optional<DbError> fatal_err;

    auto init_result_set = [&](const PGresult* res) {
        const int ncols = PQnfields(res);
        std::vector<core::ColumnSpec> specs;
        specs.reserve(static_cast<size_t>(ncols));
        col_is_binary.assign(static_cast<size_t>(ncols), false);
        for (int c = 0; c < ncols; ++c) {
            const char* fn = PQfname(res, c);
            const uint32_t oid = static_cast<uint32_t>(PQftype(res, c));
            const PgTypeEntry entry = lookup_pg_oid(oid);
            std::string_view type_name = entry.name;
            core::LogicalTypeKind kind = to_logical_type(entry.kind);
            bool may_json = entry.may_hold_json;
            if (entry.kind == PgLogicalKind::Unknown) {
                if (const auto it = custom_oid_names_.find(oid); it != custom_oid_names_.end()) {
                    type_name = it->second;
                    kind = classify_pg_type_name(type_name, may_json);
                }
            }
            col_is_binary[static_cast<size_t>(c)] = (kind == core::LogicalTypeKind::Binary);
            specs.push_back(core::ColumnSpec{
                .name          = fn != nullptr ? std::string_view{fn} : std::string_view{},
                .type_name     = type_name,
                .logical_type  = kind,
                .may_hold_json = may_json,
            });
        }
        current_rs = out.create_result_set(specs);
        if (current_rs != nullptr) {
            writer = std::make_unique<core::ResultSetWriter>(out, *current_rs, opts.max_arena_bytes, opts.max_cell_bytes);
            last_flush_tp = std::chrono::steady_clock::now();
            sink.notify_if_unscheduled();
        }
    };

    auto append_pg_tuple = [&](const PGresult* res, int r) {
        if (opts.discard_results) {
            const uint64_t total = out.total_rows_read.fetch_add(1, std::memory_order_acq_rel) + 1;
            (void)sink.on_rows_published(total);
            return;
        }
        if (writer == nullptr || writer->arena_limit_hit()) {
            return;
        }
        if (!writer->begin_row()) {
            return;
        }
        const int ncols = (std::min)(PQnfields(res), static_cast<int>(current_rs->col_count));
        for (int c = 0; c < ncols; ++c) {
            if (PQgetisnull(res, r, c) != 0) {
                writer->write_null(static_cast<uint32_t>(c));
            } else {
                const char* val = PQgetvalue(res, r, c);
                const int   len = PQgetlength(res, r, c);
                writer->write_utf8(
                    static_cast<uint32_t>(c),
                    val,
                    static_cast<size_t>((std::max)(0, len)),
                    col_is_binary[static_cast<size_t>(c)]);
            }
        }
        const uint64_t rs_rows = writer->staged_and_published_rows() + 1;
        const auto now = std::chrono::steady_clock::now();
        const bool milestone_flush =
            (rs_rows == 1 || rs_rows == 30 || rs_rows == 100) ||
            (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_tp).count() >= 100);
        const uint64_t after_rows = writer->end_row(milestone_flush);
        if (writer->staged_row_count() == 0) {
            last_flush_tp = now;
            (void)sink.on_rows_published(out.total_rows_read.load(std::memory_order_acquire));
        } else {
            (void)after_rows;
        }
    };

    auto finish_current_rs = [&]() {
        if (writer != nullptr) {
            writer->finish();
            writer.reset();
        } else if (current_rs != nullptr) {
            current_rs->complete.store(true, std::memory_order_release);
        }
        current_rs = nullptr;
        sink.notify_if_unscheduled();
    };

    for (;;) {
        while (PQisBusy(raw_conn) != 0) {
            if (cancel.is_canceled()) {
                send_cancel_once();
            }
            if (has_timeout && !timeout_fired &&
                (std::chrono::steady_clock::now() - start_tp) > timeout_dur) {
                timeout_fired = true;
                send_cancel_once();
            }
            const int sock = PQsocket(raw_conn);
            (void)wait_socket(sock, true, false, 50);
            if (cancel.is_canceled()) {
                send_cancel_once();
            }
            if (PQconsumeInput(raw_conn) == 0) {
                finish_current_rs();
                out.has_error.store(true, std::memory_order_release);
                return std::unexpected(error_from_result(nullptr, raw_conn));
            }
        }

        PGresult* raw_res = PQgetResult(raw_conn);
        if (raw_res == nullptr) {
            break;
        }
        PgResultPtr res(raw_res);
        const ExecStatusType st = PQresultStatus(res.get());

        if (cancel.is_canceled()) {
            send_cancel_once();
        }

        if (st == PGRES_SINGLE_TUPLE) {
            if (cancel.is_canceled()) {
                continue;
            }
            if (current_rs == nullptr) {
                init_result_set(res.get());
            }
            append_pg_tuple(res.get(), 0);
        } else if (st == PGRES_TUPLES_OK) {
            if (cancel.is_canceled()) {
                finish_current_rs();
                continue;
            }
            if (current_rs == nullptr && PQnfields(res.get()) > 0) {
                init_result_set(res.get());
            }
            const int ntuples = PQntuples(res.get());
            for (int r = 0; r < ntuples; ++r) {
                if (cancel.is_canceled()) {
                    send_cancel_once();
                    break;
                }
                append_pg_tuple(res.get(), r);
            }
            finish_current_rs();
        } else if (st == PGRES_COMMAND_OK) {
            const char* cmd_tuples = PQcmdTuples(res.get());
            if (cmd_tuples != nullptr && *cmd_tuples != '\0') {
                char* endp = nullptr;
                const long long affected = std::strtoll(cmd_tuples, &endp, 10);
                if (endp != cmd_tuples) {
                    out.add_rows_affected(static_cast<int64_t>(affected), current_rs);
                }
            }
            finish_current_rs();
        } else if (st == PGRES_EMPTY_QUERY) {
            finish_current_rs();
        } else if (st == PGRES_FATAL_ERROR) {
            finish_current_rs();
            if (!fatal_err.has_value()) {
                fatal_err = error_from_result(res.get(), raw_conn);
                if (cancel.is_canceled()) {
                    fatal_err->is_canceled = true;
                } else if (timeout_fired) {
                    fatal_err->message = "Query read timeout exceeded (" +
                                         std::to_string(opts.command_timeout_sec) + " s).";
                }
            }
        } else if (st == PGRES_COPY_IN || st == PGRES_COPY_OUT || st == PGRES_COPY_BOTH) {
            finish_current_rs();
            if (st == PGRES_COPY_IN) {
                (void)PQputCopyEnd(raw_conn, "COPY FROM STDIN is not supported in query tabs.");
            }
            if (!fatal_err.has_value()) {
                fatal_err = DbError{.message = "COPY streaming commands are not supported in the query editor."};
            }
        }
    }

    finish_current_rs();

    if (fatal_err.has_value()) {
        if (fatal_err->is_canceled || cancel.is_canceled()) {
            fatal_err->is_canceled = true;
            out.canceled.store(true, std::memory_order_release);
        } else {
            out.has_error.store(true, std::memory_order_release);
        }
        return std::unexpected(std::move(*fatal_err));
    }
    if (cancel.is_canceled()) {
        out.canceled.store(true, std::memory_order_release);
        return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
    }
    return {};
}

std::expected<void, DbError> PgDriver::parse_only(
    std::string_view sql,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Parse canceled.", .is_canceled = true});
    }
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) {
        return std::unexpected(DbError{.message = "Not connected to PostgreSQL."});
    }

    PGconn* raw_conn = conn_.get();
    const PGTransactionStatusType tx = PQtransactionStatus(raw_conn);
    if (tx == PQTRANS_INERROR) {
        return std::unexpected(DbError{
            .sql_state = "25P02",
            .message   = "Not parsed: the current transaction has failed. Run ROLLBACK first.",
        });
    }

    const std::vector<PgStatementPiece> pieces = split_pg_statements(sql);
    if (pieces.empty()) {
        return {};
    }

    const bool inside_tx = (tx == PQTRANS_INTRANS);
    if (inside_tx) {
        auto sp = exec_params_cancelable(raw_conn, "SAVEPOINT capybaradb_parse", {}, cancel, 15);
        if (!sp) {
            return std::unexpected(sp.error());
        }
    }

    auto rollback_savepoint = [&]() {
        if (inside_tx) {
            core::CancelToken cleanup_tok;
            (void)exec_params_cancelable(raw_conn, "ROLLBACK TO SAVEPOINT capybaradb_parse", {}, cleanup_tok, 15);
            (void)exec_params_cancelable(raw_conn, "RELEASE SAVEPOINT capybaradb_parse", {}, cleanup_tok, 15);
        }
    };

    for (const PgStatementPiece& piece : pieces) {
        if (cancel.is_canceled()) {
            rollback_savepoint();
            return std::unexpected(DbError{.message = "Parse canceled.", .is_canceled = true});
        }
        if (PQsendPrepare(raw_conn, "", piece.sql.c_str(), 0, nullptr) == 0) {
            DbError err = error_from_result(nullptr, raw_conn);
            rollback_savepoint();
            return std::unexpected(std::move(err));
        }
        ActiveCancelGuard guard(raw_conn, cancel);
        for (;;) {
            if (cancel.is_canceled()) {
                guard.fire_cancel();
            }
            if (PQconsumeInput(raw_conn) == 0) {
                DbError err = error_from_result(nullptr, raw_conn);
                rollback_savepoint();
                return std::unexpected(std::move(err));
            }
            if (PQisBusy(raw_conn) == 0) {
                break;
            }
            (void)wait_socket(PQsocket(raw_conn), true, false, 50);
        }
        PgResultPtr first_res;
        while (PGresult* raw_res = PQgetResult(raw_conn)) {
            PgResultPtr cur(raw_res);
            if (!first_res) {
                first_res = std::move(cur);
            }
        }
        if (!first_res) {
            DbError err = error_from_result(nullptr, raw_conn);
            if (cancel.is_canceled()) {
                err.is_canceled = true;
            }
            rollback_savepoint();
            return std::unexpected(std::move(err));
        }
        const ExecStatusType st = PQresultStatus(first_res.get());
        if (st != PGRES_COMMAND_OK) {
            DbError err = error_from_result(first_res.get(), raw_conn);
            if (err.position > 0) {
                err.position += piece.char_offset;
            }
            if (cancel.is_canceled()) {
                err.is_canceled = true;
            }
            rollback_savepoint();
            return std::unexpected(std::move(err));
        }
    }

    if (inside_tx) {
        auto rel = exec_params_cancelable(raw_conn, "RELEASE SAVEPOINT capybaradb_parse", {}, cancel, 15);
        if (!rel) {
            return std::unexpected(rel.error());
        }
    }
    return {};
}

std::expected<std::vector<core::RawExecutionPlan>, DbError> PgDriver::explain_plan(
    std::string_view sql,
    bool analyze,
    uint32_t timeout_sec,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Execution plan canceled.", .is_canceled = true});
    }
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) {
        return std::unexpected(DbError{.message = "Not connected to PostgreSQL."});
    }

    PGconn* raw_conn = conn_.get();
    if (plan_capture_armed_) {
        core::CancelToken disarm_tok;
        (void)exec_params_cancelable(raw_conn, "SELECT pg_catalog.set_config('auto_explain.log_min_duration','-1',false)", {}, disarm_tok, 10);
        plan_capture_armed_ = false;
    }

    const std::vector<PgStatementPiece> pieces = split_pg_statements(sql);
    if (pieces.empty()) {
        return std::unexpected(DbError{.message = "Enter a SQL statement to explain."});
    }

    std::vector<core::RawExecutionPlan> plans;
    plans.reserve(pieces.size());
    const std::string prefix = analyze
        ? "EXPLAIN (ANALYZE TRUE, BUFFERS TRUE, FORMAT JSON) "
        : "EXPLAIN (ANALYZE FALSE, FORMAT JSON) ";

    for (const PgStatementPiece& piece : pieces) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Execution plan canceled.", .is_canceled = true});
        }
        const std::string explain_sql = prefix + piece.sql;
        auto res = exec_params_cancelable(raw_conn, explain_sql.c_str(), {}, cancel, timeout_sec);
        if (!res) {
            DbError err = res.error();
            if (err.position > static_cast<int32_t>(prefix.size())) {
                err.position = err.position - static_cast<int32_t>(prefix.size()) + piece.char_offset;
            }
            return std::unexpected(std::move(err));
        }
        if (PQnfields(res->get()) < 1 || PQntuples(res->get()) < 1) {
            return std::unexpected(DbError{.message = "Server returned no execution plan."});
        }
        std::string json;
        const int nrows = PQntuples(res->get());
        for (int r = 0; r < nrows; ++r) {
            if (PQgetisnull(res->get(), r, 0) == 0) {
                const char* val = PQgetvalue(res->get(), r, 0);
                const int len   = PQgetlength(res->get(), r, 0);
                if (val != nullptr && len > 0) {
                    json.append(val, static_cast<size_t>(len));
                }
            }
        }
        if (json.size() > core::kMaxExecutionPlanBytes) {
            return std::unexpected(DbError{.message = "Plan exceeds display limit; no partial plan is displayed."});
        }
        plans.push_back(core::RawExecutionPlan{
            .source_kind = core::PlanSourceKind::PostgreSqlJson,
            .raw_text    = std::move(json),
            .analyzed    = analyze,
            .query_text  = piece.sql,
        });
    }
    return plans;
}

} // namespace capybaradb::drivers
