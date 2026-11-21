#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include "core/config/utf_convert.hpp"
#include "drivers/common/driver_registry.hpp"
#include "drivers/mssql/mssql_batch_splitter.hpp"
#include "drivers/mssql/mssql_driver.hpp"
#include "drivers/mssql/mssql_type_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace capybaradb::drivers {
namespace {

using SqlWString = std::basic_string<SQLWCHAR>;

SqlWString to_sqlw(std::string_view utf8) {
    const std::u16string u16 = core::utf8_to_utf16(utf8);
    SqlWString out(u16.size(), static_cast<SQLWCHAR>(0));
    for (size_t i = 0; i < u16.size(); ++i) {
        out[i] = static_cast<SQLWCHAR>(u16[i]);
    }
    return out;
}

std::string from_sqlw(const SQLWCHAR* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return {};
    }
    std::u16string u16(len, u'\0');
    for (size_t i = 0; i < len; ++i) {
        u16[i] = static_cast<char16_t>(buf[i]);
    }
    return core::utf16_to_utf8(u16);
}

std::string strip_odbc_prefixes(std::string msg) {
    while (msg.starts_with('[')) {
        const size_t close = msg.find(']');
        if (close == std::string::npos) {
            break;
        }
        msg.erase(0, close + 1);
    }
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

DbError extract_diag(SQLSMALLINT handle_type, SQLHANDLE handle) {
    DbError err;
    if (handle == SQL_NULL_HANDLE) {
        err.message = "Unknown ODBC error.";
        return err;
    }

    SQLWCHAR state[6]{};
    SQLINTEGER native = 0;
    SQLWCHAR text[1024]{};
    SQLSMALLINT text_len = 0;

    std::string combined;
    SQLSMALLINT rec = 1;
    while (SQL_SUCCEEDED(SQLGetDiagRecW(
        handle_type,
        handle,
        rec,
        state,
        &native,
        text,
        static_cast<SQLSMALLINT>(std::size(text)),
        &text_len))) {
        if (rec == 1) {
            err.sql_state   = from_sqlw(state, 5);
            err.native_code = static_cast<int32_t>(native);
        }
        const size_t clen = (text_len > 0 && static_cast<size_t>(text_len) < std::size(text))
            ? static_cast<size_t>(text_len)
            : 0;
        std::string part = strip_odbc_prefixes(from_sqlw(text, clen));
        if (!part.empty()) {
            if (!combined.empty()) {
                combined += '\n';
            }
            combined += part;
        }
        ++rec;
    }

    err.message = combined.empty() ? std::string{"ODBC operation failed."} : std::move(combined);
    if (err.sql_state == "HY008") {
        err.is_canceled = true;
    }
    return err;
}

std::string quote_tsql_ident(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 4);
    out.push_back('[');
    for (const char ch : ident) {
        if (ch == ']') {
            out.push_back(']');
        }
        out.push_back(ch);
    }
    out.push_back(']');
    return out;
}

void drain_odbc_notices(
    SQLHSTMT hstmt,
    core::QueryExecutionResult& out,
    core::StreamPublishSink& sink) {
    if (hstmt == SQL_NULL_HSTMT) {
        return;
    }
    SQLWCHAR state_buf[6]{};
    SQLINTEGER native = 0;
    SQLWCHAR msg_buf[1024]{};
    SQLSMALLINT msg_len = 0;
    SQLSMALLINT rec = 1;
    bool added = false;

    while (SQL_SUCCEEDED(SQLGetDiagRecW(
        SQL_HANDLE_STMT,
        hstmt,
        rec,
        state_buf,
        &native,
        msg_buf,
        static_cast<SQLSMALLINT>(std::size(msg_buf)),
        &msg_len))) {
        const size_t clen = (msg_len > 0 && static_cast<size_t>(msg_len) < std::size(msg_buf))
            ? static_cast<size_t>(msg_len)
            : 0;
        std::string msg = strip_odbc_prefixes(from_sqlw(msg_buf, clen));
        if (!msg.empty()) {
            const std::string ss = from_sqlw(state_buf, 5);
            const core::MessageSeverity sev = (native == 0) ? core::MessageSeverity::Notice : core::MessageSeverity::Info;
            out.append_message(sev, ss, msg, -1, static_cast<int32_t>(native));
            added = true;
        }
        ++rec;
    }
    if (added) {
        sink.notify_if_unscheduled();
    }
}

bool is_binary_sql_type(SQLSMALLINT sql_type) noexcept {
    return is_mssql_binary_type(static_cast<int16_t>(sql_type));
}

bool is_unicode_sql_type(SQLSMALLINT sql_type) noexcept {
    return should_bind_mssql_as_wchar(static_cast<int16_t>(sql_type));
}

bool is_lob_odbc_column(SQLSMALLINT sql_type, SQLULEN col_size) noexcept {
    return is_mssql_lob_or_dynamic_type(static_cast<int16_t>(sql_type), static_cast<uint64_t>(col_size));
}

core::LogicalTypeKind classify_odbc_type(
    SQLSMALLINT sql_type,
    std::string_view type_name,
    bool& may_hold_json) noexcept {
    return classify_mssql_column(static_cast<int16_t>(sql_type), type_name, may_hold_json);
}

bool is_showplan_xml_column(std::string_view col_name) noexcept {
    return col_name.find("XML Showplan") != std::string_view::npos;
}

std::string read_full_wchar_column(SQLHSTMT hstmt, SQLUSMALLINT col, size_t max_bytes) {
    std::u16string u16;
    std::array<SQLWCHAR, 4096> chunk{};
    for (;;) {
        SQLLEN ind = 0;
        const SQLRETURN rc = SQLGetData(
            hstmt,
            col,
            SQL_C_WCHAR,
            chunk.data(),
            static_cast<SQLLEN>(chunk.size() * sizeof(SQLWCHAR)),
            &ind);
        if (rc == SQL_NO_DATA || ind == SQL_NULL_DATA || !SQL_SUCCEEDED(rc)) {
            break;
        }
        const size_t avail = chunk.size() - 1;
        const size_t nchars = (rc == SQL_SUCCESS_WITH_INFO)
            ? ((ind != SQL_NO_TOTAL && ind >= 0) ? (std::min<size_t>)(static_cast<size_t>(ind) / sizeof(SQLWCHAR), avail) : avail)
            : (ind >= 0 ? (std::min<size_t>)(static_cast<size_t>(ind) / sizeof(SQLWCHAR), avail) : 0u);
        if (u16.size() < max_bytes) {
            const size_t take = (std::min)(nchars, max_bytes - u16.size());
            const size_t old = u16.size();
            u16.resize(old + take);
            for (size_t i = 0; i < take; ++i) {
                u16[old + i] = static_cast<char16_t>(chunk[i]);
            }
        }
        if (rc == SQL_SUCCESS) break;
    }
    return core::utf16_to_utf8(u16);
}

void stream_lob_binary_cell(
    SQLHSTMT hstmt,
    SQLUSMALLINT col,
    uint32_t col_idx,
    core::ResultSetWriter& writer,
    std::vector<uint8_t>& scratch,
    uint32_t max_cell_bytes,
    bool decode_hierarchyid = false) {
    scratch.clear();
    const size_t max_raw_bytes = (max_cell_bytes > 2u) ? (max_cell_bytes - 2u) / 2u : 0u;
    std::array<uint8_t, 8192> chunk{};
    bool saw_data = false;

    for (;;) {
        SQLLEN ind = 0;
        const SQLRETURN rc = SQLGetData(
            hstmt,
            col,
            SQL_C_BINARY,
            chunk.data(),
            static_cast<SQLLEN>(chunk.size()),
            &ind);
        if (rc == SQL_NO_DATA) {
            break;
        }
        if (ind == SQL_NULL_DATA) {
            writer.write_null(col_idx);
            return;
        }
        if (!SQL_SUCCEEDED(rc)) {
            writer.write_utf8(col_idx, "", 0, true);
            return;
        }
        saw_data = true;
        const size_t nbytes = (rc == SQL_SUCCESS_WITH_INFO)
            ? ((ind != SQL_NO_TOTAL && ind >= 0) ? (std::min<size_t>)(static_cast<size_t>(ind), chunk.size()) : chunk.size())
            : (ind >= 0 ? (std::min<size_t>)(static_cast<size_t>(ind), chunk.size()) : 0u);
        if (scratch.size() < max_raw_bytes + 1u) {
            const size_t take = (std::min)(nbytes, (max_raw_bytes + 1u) - scratch.size());
            scratch.insert(scratch.end(), chunk.data(), chunk.data() + take);
        }
        if (rc == SQL_SUCCESS) {
            break;
        }
    }
    if (!saw_data) {
        writer.write_null(col_idx);
        return;
    }
    if (decode_hierarchyid) {
        if (const auto decoded = decode_mssql_hierarchyid(scratch); decoded.has_value()) {
            writer.write_utf8(col_idx, decoded->data(), decoded->size(), false);
            return;
        }
    }
    writer.write_binary_hex(col_idx, scratch.data(), scratch.size(), "0x");
}

void stream_lob_wchar_cell(
    SQLHSTMT hstmt,
    SQLUSMALLINT col,
    uint32_t col_idx,
    core::ResultSetWriter& writer,
    std::vector<char16_t>& scratch,
    uint32_t max_cell_bytes) {
    scratch.clear();
    std::array<SQLWCHAR, 4096> chunk{};
    const size_t max_u16 = (max_cell_bytes > 0) ? static_cast<size_t>(max_cell_bytes) + 1u : SIZE_MAX;
    bool saw_data = false;

    for (;;) {
        SQLLEN ind = 0;
        const SQLRETURN rc = SQLGetData(
            hstmt,
            col,
            SQL_C_WCHAR,
            chunk.data(),
            static_cast<SQLLEN>(chunk.size() * sizeof(SQLWCHAR)),
            &ind);
        if (rc == SQL_NO_DATA) {
            break;
        }
        if (ind == SQL_NULL_DATA) {
            writer.write_null(col_idx);
            return;
        }
        if (!SQL_SUCCEEDED(rc)) {
            writer.write_utf8(col_idx, "", 0, false);
            return;
        }
        saw_data = true;
        const size_t avail_wchars = chunk.size() - 1;
        const size_t nchars = (rc == SQL_SUCCESS_WITH_INFO)
            ? ((ind != SQL_NO_TOTAL && ind >= 0)
                   ? (std::min<size_t>)(static_cast<size_t>(ind) / sizeof(SQLWCHAR), avail_wchars)
                   : avail_wchars)
            : (ind >= 0 ? (std::min<size_t>)(static_cast<size_t>(ind) / sizeof(SQLWCHAR), avail_wchars) : 0u);

        if (scratch.size() < max_u16) {
            const size_t take = (std::min)(nchars, max_u16 - scratch.size());
            const size_t old_sz = scratch.size();
            scratch.resize(old_sz + take);
            for (size_t i = 0; i < take; ++i) {
                scratch[old_sz + i] = static_cast<char16_t>(chunk[i]);
            }
        }
        if (rc == SQL_SUCCESS) {
            break;
        }
    }
    if (!saw_data) {
        writer.write_null(col_idx);
        return;
    }
    writer.write_utf16(col_idx, scratch.data(), scratch.size());
}

} // namespace

struct MsSqlDriver::OdbcSession {
    SQLHENV henv      = SQL_NULL_HENV;
    SQLHDBC hdbc      = SQL_NULL_HDBC;
    bool    connected = false;
    mutable std::mutex cancel_mu;
    SQLHSTMT active_stmt = SQL_NULL_HSTMT;

    OdbcSession() = default;
    OdbcSession(const OdbcSession&) = delete;
    OdbcSession& operator=(const OdbcSession&) = delete;

    OdbcSession(OdbcSession&& other) noexcept
        : henv(other.henv)
        , hdbc(other.hdbc)
        , connected(other.connected)
        , active_stmt(other.active_stmt) {
        other.henv        = SQL_NULL_HENV;
        other.hdbc        = SQL_NULL_HDBC;
        other.connected   = false;
        other.active_stmt = SQL_NULL_HSTMT;
    }

    OdbcSession& operator=(OdbcSession&& other) noexcept {
        if (this != &other) {
            close();
            henv        = other.henv;
            hdbc        = other.hdbc;
            connected   = other.connected;
            active_stmt = other.active_stmt;
            other.henv        = SQL_NULL_HENV;
            other.hdbc        = SQL_NULL_HDBC;
            other.connected   = false;
            other.active_stmt = SQL_NULL_HSTMT;
        }
        return *this;
    }

    ~OdbcSession() {
        close();
    }

    void close() noexcept {
        if (hdbc != SQL_NULL_HDBC) {
            if (connected) {
                SQLDisconnect(hdbc);
            }
            SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            hdbc      = SQL_NULL_HDBC;
            connected = false;
        }
        if (henv != SQL_NULL_HENV) {
            SQLFreeHandle(SQL_HANDLE_ENV, henv);
            henv = SQL_NULL_HENV;
        }
    }

    void cancel() noexcept {
        std::lock_guard<std::mutex> lock(cancel_mu);
        if (active_stmt != SQL_NULL_HSTMT) {
            // SQLCancel works on both Windows built-in SQL Server ODBC 3.5 (sqlsrv32.dll)
            // and ODBC Driver 17/18 for SQL Server. Only active during server-side
            // SQLExecDirectW / SQLMoreResults (never concurrently with SQLFetch / SQLGetData).
            SQLCancel(active_stmt);
        } else if (!connected && hdbc != SQL_NULL_HDBC) {
            SQLCancelHandle(SQL_HANDLE_DBC, hdbc);
        }
    }
};

std::vector<std::string> MsSqlDriver::list_odbc_drivers() {
    std::vector<std::string> out;
    SQLHENV henv = SQL_NULL_HENV;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv))) {
        return out;
    }
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0))) {
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    }

    std::array<SQLWCHAR, 512> desc{};
    std::array<SQLWCHAR, 1024> attrs{};
    SQLSMALLINT desc_len = 0;
    SQLSMALLINT attr_len = 0;

    SQLUSMALLINT dir = SQL_FETCH_FIRST;
    while (SQL_SUCCEEDED(SQLDriversW(
        henv,
        dir,
        desc.data(),
        static_cast<SQLSMALLINT>(desc.size()),
        &desc_len,
        attrs.data(),
        static_cast<SQLSMALLINT>(attrs.size()),
        &attr_len))) {
        dir = SQL_FETCH_NEXT;
        if (desc_len > 0) {
            const size_t n = std::min(static_cast<size_t>(desc_len), desc.size());
            out.push_back(from_sqlw(desc.data(), n));
        }
    }

    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    return out;
}

std::expected<MsSqlDriver::OdbcSession, DbError> MsSqlDriver::open_odbc_session(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Connection canceled.", .is_canceled = true});
    }

    const std::string driver_name = detect_mssql_odbc_driver();
    if (driver_name.empty()) {
        return std::unexpected(DbError{
            .sql_state = "IM002",
            .message = "No Microsoft SQL Server ODBC driver (18 or 17) is installed on this system. "
                       "Install 'ODBC Driver 18 for SQL Server' (msodbcsql18) to connect to SQL Server.",
        });
    }

    OdbcSession sess;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &sess.henv))) {
        return std::unexpected(DbError{.message = "Failed to allocate ODBC environment handle."});
    }
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(sess.henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0))) {
        SQLSetEnvAttr(sess.henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, sess.henv, &sess.hdbc))) {
        return std::unexpected(extract_diag(SQL_HANDLE_ENV, sess.henv));
    }

    const SQLULEN login_timeout = cfg.connect_timeout_sec > 0 ? cfg.connect_timeout_sec : 10;
    SQLSetConnectAttrW(sess.hdbc, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(login_timeout)), 0);

    core::ScopedCancelCallback cancel_guard(cancel, [&sess]() {
        sess.cancel();
    });

    const std::string raw_conn = build_connection_string(cfg, driver_name);
    const SqlWString wconn = to_sqlw(raw_conn);

    std::array<SQLWCHAR, 1024> out_buf{};
    SQLSMALLINT out_len = 0;
    const SQLRETURN rc = SQLDriverConnectW(
        sess.hdbc,
        nullptr,
        const_cast<SQLWCHAR*>(wconn.c_str()),
        static_cast<SQLSMALLINT>(wconn.size()),
        out_buf.data(),
        static_cast<SQLSMALLINT>(out_buf.size()),
        &out_len,
        SQL_DRIVER_NOPROMPT);

    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Connection canceled.", .is_canceled = true});
    }
    if (!SQL_SUCCEEDED(rc)) {
        return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess.hdbc));
    }

    sess.connected = true;
    return sess;
}

MsSqlDriver::~MsSqlDriver() {
    disconnect();
}

std::shared_ptr<MsSqlDriver::OdbcSession> MsSqlDriver::acquire_session() const {
    std::lock_guard<std::mutex> lock(conn_mu_);
    return session_;
}

void MsSqlDriver::disconnect() noexcept {
    std::shared_ptr<OdbcSession> old;
    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        old = std::move(session_);
        session_info_ = {};
    }
    if (old) {
        old->cancel();
        old->close();
    }
}

bool MsSqlDriver::is_connected() const noexcept {
    const auto sess = acquire_session();
    return sess && sess->connected;
}

SessionInfo MsSqlDriver::session_info() const {
    std::lock_guard<std::mutex> lock(conn_mu_);
    return session_info_;
}

void MsSqlDriver::cancel_active() noexcept {
    const auto sess = acquire_session();
    if (sess) {
        sess->cancel();
    }
}

std::expected<SessionInfo, DbError> MsSqlDriver::query_session_info(
    OdbcSession& sess,
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess.hdbc, &hstmt))) {
        return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess.hdbc));
    }

    {
        std::lock_guard<std::mutex> lk(sess.cancel_mu);
        sess.active_stmt = hstmt;
    }
    core::ScopedCancelCallback cancel_guard(cancel, [&sess]() { sess.cancel(); });

    const SqlWString wsql = to_sqlw("SELECT CAST(@@SPID AS BIGINT), @@VERSION, DB_NAME(), SUSER_SNAME()");
    const SQLRETURN rc = SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLSMALLINT>(wsql.size()));

    auto cleanup = [&]() {
        {
            std::lock_guard<std::mutex> lk(sess.cancel_mu);
            sess.active_stmt = SQL_NULL_HSTMT;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    };

    if (!SQL_SUCCEEDED(rc)) {
        DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
        cleanup();
        return std::unexpected(std::move(err));
    }

    SessionInfo info;
    info.engine = EngineKind::SqlServer;
    info.host   = cfg.host;
    info.port   = cfg.port;

    if (SQL_SUCCEEDED(SQLFetch(hstmt))) {
        SQLBIGINT spid = 0;
        SQLLEN ind0 = 0;
        if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SBIGINT, &spid, sizeof(spid), &ind0)) && ind0 != SQL_NULL_DATA) {
            info.session_id = static_cast<int64_t>(spid);
        }

        auto get_str = [&](SQLUSMALLINT col) -> std::string {
            std::array<SQLWCHAR, 1024> buf{};
            SQLLEN ind = 0;
            if (SQL_SUCCEEDED(SQLGetData(hstmt, col, SQL_C_WCHAR, buf.data(), static_cast<SQLLEN>(buf.size() * sizeof(SQLWCHAR)), &ind)) && ind > 0) {
                const size_t chars = std::min(static_cast<size_t>(ind / sizeof(SQLWCHAR)), buf.size() - 1);
                return from_sqlw(buf.data(), chars);
            }
            return {};
        };

        info.server_version = get_str(2);
        info.database       = get_str(3);
        info.username       = get_str(4);
    }

    cleanup();
    return info;
}

std::expected<void, DbError> MsSqlDriver::connect(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    auto opened = open_odbc_session(cfg, cancel);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto info = query_session_info(*opened, cfg, cancel);
    if (!info) {
        return std::unexpected(info.error());
    }

    std::lock_guard<std::mutex> lock(conn_mu_);
    session_      = std::make_shared<OdbcSession>(std::move(*opened));
    current_cfg_  = cfg;
    session_info_ = std::move(*info);
    return {};
}

std::expected<ConnectionTestResult, DbError> MsSqlDriver::test_connection(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    auto opened = open_odbc_session(cfg, cancel);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto info = query_session_info(*opened, cfg, cancel);
    if (!info) {
        return std::unexpected(info.error());
    }

    ConnectionTestResult out;
    out.database   = info->database;
    out.user       = info->username;
    out.version    = info->server_version;
    out.tls_active = (cfg.tls_mode != TlsMode::Disable);
    return out;
}

std::expected<void, DbError> MsSqlDriver::switch_database(
    std::string_view database,
    core::CancelToken& cancel) {
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return std::unexpected(DbError{.message = "Not connected."});
    }

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
        return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
    }

    {
        std::lock_guard<std::mutex> lk(sess->cancel_mu);
        sess->active_stmt = hstmt;
    }
    core::ScopedCancelCallback cancel_guard(cancel, [sess]() {
        sess->cancel();
    });

    const std::string use_sql = "USE " + quote_tsql_ident(database);
    const SqlWString wsql = to_sqlw(use_sql);
    const SQLRETURN rc = SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLSMALLINT>(wsql.size()));

    {
        std::lock_guard<std::mutex> lk(sess->cancel_mu);
        sess->active_stmt = SQL_NULL_HSTMT;
    }

    if (!SQL_SUCCEEDED(rc)) {
        DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return std::unexpected(std::move(err));
    }

    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    std::lock_guard<std::mutex> lock(conn_mu_);
    current_cfg_.database  = std::string(database);
    session_info_.database = std::string(database);
    return {};
}

std::expected<SessionLossRisk, DbError> MsSqlDriver::inspect_loss_risk(
    core::CancelToken& cancel) {
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return SessionLossRisk{};
    }

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
        return SessionLossRisk{};
    }
    core::ScopedCancelCallback cancel_guard(cancel, [sess]() {
        sess->cancel();
    });

    SessionLossRisk risk;
    const SqlWString wsql = to_sqlw(
        "SELECT CAST(@@TRANCOUNT AS INT), "
        "CASE WHEN EXISTS (SELECT 1 FROM tempdb.sys.objects WHERE object_id > 0 AND name LIKE '#%') THEN 1 ELSE 0 END");
    if (SQL_SUCCEEDED(SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLSMALLINT>(wsql.size())))
        && SQL_SUCCEEDED(SQLFetch(hstmt))) {
        SQLINTEGER tran_cnt = 0;
        SQLINTEGER has_temp = 0;
        SQLLEN i0 = 0;
        SQLLEN i1 = 0;
        if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SLONG, &tran_cnt, sizeof(tran_cnt), &i0)) && i0 != SQL_NULL_DATA) {
            risk.open_transaction = (tran_cnt > 0);
        }
        if (SQL_SUCCEEDED(SQLGetData(hstmt, 2, SQL_C_SLONG, &has_temp, sizeof(has_temp), &i1)) && i1 != SQL_NULL_DATA) {
            risk.temporary_tables = (has_temp != 0);
        }
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return risk;
}

std::expected<void, DbError> MsSqlDriver::commit_transaction(
    core::CancelToken& cancel) {
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return std::unexpected(DbError{.message = "Not connected."});
    }
    core::ScopedCancelCallback cancel_guard(cancel, [sess]() {
        sess->cancel();
    });

    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
        return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
    }
    const SqlWString wsql = to_sqlw("IF @@TRANCOUNT > 0 COMMIT TRANSACTION");
    const SQLRETURN rc = SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLSMALLINT>(wsql.size()));
    if (!SQL_SUCCEEDED(rc)) {
        DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return std::unexpected(std::move(err));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return {};
}

std::expected<void, DbError> MsSqlDriver::execute_stream(
    std::string_view sql,
    const ExecOptions& opts,
    core::QueryExecutionResult& out,
    core::StreamPublishSink& sink,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        out.canceled.store(true, std::memory_order_release);
        return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
    }
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return std::unexpected(DbError{.message = "Not connected to SQL Server."});
    }

    static constexpr SQLULEN kBlockFetchRows = 256u;

    enum class BindKind : uint8_t { Char, WChar, Binary };
    struct BoundColBuffer {
        BindKind            kind       = BindKind::Char;
        size_t              elem_bytes = 0;
        std::vector<uint8_t> data;
        std::vector<SQLLEN>  indicators;
    };

    auto exec_control_stmt = [&](std::string_view cmd) -> std::expected<void, DbError> {
        SQLHSTMT cstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &cstmt))) {
            return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
        }
        const SqlWString wcmd = to_sqlw(cmd);
        const SQLRETURN crc = SQLExecDirectW(cstmt, const_cast<SQLWCHAR*>(wcmd.c_str()), static_cast<SQLINTEGER>(wcmd.size()));
        if (!SQL_SUCCEEDED(crc)) {
            DbError err = extract_diag(SQL_HANDLE_STMT, cstmt);
            SQLFreeHandle(SQL_HANDLE_STMT, cstmt);
            return std::unexpected(std::move(err));
        }
        while (SQLMoreResults(cstmt) != SQL_NO_DATA) {}
        SQLFreeHandle(SQL_HANDLE_STMT, cstmt);
        return {};
    };

    if (opts.isolation != core::QueryIsolationLevel::ServerDefault) {
        const char* iso_cmd = nullptr;
        switch (opts.isolation) {
        case core::QueryIsolationLevel::ReadCommitted:  iso_cmd = "SET TRANSACTION ISOLATION LEVEL READ COMMITTED"; break;
        case core::QueryIsolationLevel::RepeatableRead: iso_cmd = "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ"; break;
        case core::QueryIsolationLevel::Serializable:   iso_cmd = "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"; break;
        default: break;
        }
        if (iso_cmd != nullptr) {
            (void)exec_control_stmt(iso_cmd);
        }
    }
    if (opts.lock_timeout_ms >= 0) {
        (void)exec_control_stmt("SET LOCK_TIMEOUT " + std::to_string(opts.lock_timeout_ms));
    }

    bool stats_xml_armed = false;
    if (opts.include_actual_plan) {
        if (auto on_res = exec_control_stmt("SET STATISTICS XML ON"); on_res) {
            stats_xml_armed = true;
        } else {
            out.set_actual_plan_unavailable(on_res.error().format_short());
        }
    }
    struct StatsXmlGuard final {
        const decltype(exec_control_stmt)& fn;
        bool armed = false;
        ~StatsXmlGuard() {
            if (armed) {
                (void)fn("SET STATISTICS XML OFF");
            }
        }
    } stats_xml_guard{exec_control_stmt, stats_xml_armed};

    const std::vector<TSqlBatch> batches = split_tsql_batches(sql);
    for (const TSqlBatch& batch : batches) {
        const uint32_t reps = batch.repeat_count > 0 ? batch.repeat_count : 1u;
        const SqlWString wsql = to_sqlw(batch.sql);

        for (uint32_t rep = 0; rep < reps; ++rep) {
            if (cancel.is_canceled()) {
                out.canceled.store(true, std::memory_order_release);
                return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
            }

            SQLHSTMT hstmt = SQL_NULL_HSTMT;
            if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
                out.has_error.store(true, std::memory_order_release);
                return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
            }

            if (opts.command_timeout_sec > 0) {
                SQLSetStmtAttrW(
                    hstmt,
                    SQL_ATTR_QUERY_TIMEOUT,
                    reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(opts.command_timeout_sec)),
                    0);
            }
            SQLSetStmtAttrW(
                hstmt,
                SQL_ATTR_CURSOR_TYPE,
                reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_CURSOR_FORWARD_ONLY)),
                0);

            {
                std::lock_guard<std::mutex> lk(sess->cancel_mu);
                sess->active_stmt = hstmt;
            }
            struct StmtCleanup final {
                std::shared_ptr<OdbcSession> sess;
                SQLHSTMT                     hstmt  = SQL_NULL_HSTMT;
                const core::CancelToken*     cancel = nullptr;
                ~StmtCleanup() noexcept {
                    if (sess) {
                        std::lock_guard<std::mutex> lk(sess->cancel_mu);
                        if (sess->active_stmt == hstmt) {
                            sess->active_stmt = SQL_NULL_HSTMT;
                        }
                    }
                    if (hstmt != SQL_NULL_HSTMT) {
                        if (cancel != nullptr && cancel->is_canceled()) {
                            SQLCancel(hstmt);
                        }
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                    }
                }
            } stmt_cleanup{sess, hstmt, &cancel};

            SQLRETURN rc = SQLExecDirectW(
                hstmt,
                const_cast<SQLWCHAR*>(wsql.c_str()),
                static_cast<SQLINTEGER>(wsql.size()));

            {
                std::lock_guard<std::mutex> lk(sess->cancel_mu);
                if (sess->active_stmt == hstmt) {
                    sess->active_stmt = SQL_NULL_HSTMT;
                }
            }

            if (rc == SQL_SUCCESS_WITH_INFO) {
                drain_odbc_notices(hstmt, out, sink);
            }
            if (!SQL_SUCCEEDED(rc)) {
                DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
                if (cancel.is_canceled()) {
                    err.is_canceled = true;
                    out.canceled.store(true, std::memory_order_release);
                } else {
                    out.has_error.store(true, std::memory_order_release);
                }
                if (err.position <= 0) {
                    err.position = static_cast<int32_t>(batch.start_line);
                } else {
                    err.position += static_cast<int32_t>(batch.start_line - 1);
                }
                return std::unexpected(std::move(err));
            }

            for (;;) {
                SQLSMALLINT num_cols = 0;
                const SQLRETURN cols_rc = SQLNumResultCols(hstmt, &num_cols);
                if (!SQL_SUCCEEDED(cols_rc)) {
                    DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
                    out.has_error.store(true, std::memory_order_release);
                    return std::unexpected(std::move(err));
                }

                if (num_cols > 0) {
                    std::vector<std::string>      col_names;
                    std::vector<std::string>      col_types;
                    std::vector<SQLSMALLINT>      sql_types;
                    std::vector<SQLULEN>          col_sizes;
                    std::vector<core::ColumnSpec> specs;
                    col_names.reserve(static_cast<size_t>(num_cols));
                    col_types.reserve(static_cast<size_t>(num_cols));
                    sql_types.reserve(static_cast<size_t>(num_cols));
                    col_sizes.reserve(static_cast<size_t>(num_cols));
                    specs.reserve(static_cast<size_t>(num_cols));

                    bool has_lob = false;
                    for (SQLUSMALLINT c = 1; c <= static_cast<SQLUSMALLINT>(num_cols); ++c) {
                        std::array<SQLWCHAR, 256> cname{};
                        SQLSMALLINT cname_len = 0;
                        SQLSMALLINT dtype = 0;
                        SQLULEN     csize = 0;
                        SQLSMALLINT scale = 0;
                        SQLSMALLINT nullable = 0;
                        SQLDescribeColW(
                            hstmt,
                            c,
                            cname.data(),
                            static_cast<SQLSMALLINT>(cname.size()),
                            &cname_len,
                            &dtype,
                            &csize,
                            &scale,
                            &nullable);
                        const size_t ncl = (cname_len > 0 && static_cast<size_t>(cname_len) < cname.size())
                            ? static_cast<size_t>(cname_len)
                            : 0;
                        col_names.push_back(from_sqlw(cname.data(), ncl));
                        sql_types.push_back(dtype);
                        col_sizes.push_back(csize);
                        if (is_lob_odbc_column(dtype, csize)) {
                            has_lob = true;
                        }

                        std::array<SQLWCHAR, 128> tname{};
                        SQLSMALLINT tlen = 0;
                        if (SQL_SUCCEEDED(SQLColAttributeW(
                                hstmt,
                                c,
                                SQL_DESC_TYPE_NAME,
                                tname.data(),
                                static_cast<SQLSMALLINT>(tname.size() * sizeof(SQLWCHAR)),
                                &tlen,
                                nullptr)) &&
                            tlen > 0) {
                            const size_t tchars = (std::min<size_t>)(
                                static_cast<size_t>(tlen) / sizeof(SQLWCHAR),
                                tname.size() - 1);
                            col_types.push_back(from_sqlw(tname.data(), tchars));
                        } else {
                            col_types.emplace_back();
                        }
                    }

                    // Intercept SQL Server Actual Execution Plan (SET STATISTICS XML ON) result sets
                    if (stats_xml_armed && num_cols == 1 && is_showplan_xml_column(col_names[0])) {
                        while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
                            std::string xml = read_full_wchar_column(hstmt, 1, core::kMaxExecutionPlanBytes);
                            if (!xml.empty()) {
                                out.add_actual_plan(core::RawExecutionPlan{
                                    .source_kind = core::PlanSourceKind::SqlServerXml,
                                    .raw_text    = std::move(xml),
                                    .analyzed    = true,
                                    .query_text  = batch.sql,
                                });
                            }
                        }
                        if (cancel.is_canceled()) {
                            out.canceled.store(true, std::memory_order_release);
                            return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                        }
                        const SQLRETURN plan_more = SQLMoreResults(hstmt);
                        if (plan_more == SQL_NO_DATA) break;
                        continue;
                    }

                    std::vector<bool> col_is_hierarchyid(static_cast<size_t>(num_cols), false);
                    for (size_t c = 0; c < static_cast<size_t>(num_cols); ++c) {
                        bool may_json = false;
                        const core::LogicalTypeKind kind = classify_odbc_type(sql_types[c], col_types[c], may_json);
                        col_is_hierarchyid[c] = (ascii_lower_copy(col_types[c]) == "hierarchyid");
                        specs.push_back(core::ColumnSpec{
                            .name          = col_names[c],
                            .type_name     = col_types[c],
                            .logical_type  = kind,
                            .may_hold_json = may_json,
                        });
                    }

                    core::ResultSetStorage* rs = out.create_result_set(specs);
                    if (rs != nullptr) {
                        sink.notify_if_unscheduled();
                    }
                    std::unique_ptr<core::ResultSetWriter> writer = (rs != nullptr)
                        ? std::make_unique<core::ResultSetWriter>(out, *rs, opts.max_arena_bytes, opts.max_cell_bytes)
                        : nullptr;
                    const uint32_t stored_cols = (rs != nullptr) ? rs->col_count : 0u;

                    if (!has_lob && !opts.discard_results && writer != nullptr && stored_cols > 0) {
                        // Fast Path: Column-Wise Block Fetch (up to 256 rows, capped to ~16 MiB total bind buffer
                        // so wide 300-column tables with large VARCHAR/NVARCHAR columns never cause huge heap spikes).
                        std::vector<BoundColBuffer> cols(stored_cols);
                        size_t row_stride_bytes = 0;
                        for (uint32_t c = 0; c < stored_cols; ++c) {
                            const SQLSMALLINT dtype = sql_types[c];
                            const SQLULEN     csize = col_sizes[c];
                            BoundColBuffer& cb = cols[c];
                            if (is_binary_sql_type(dtype)) {
                                cb.kind       = BindKind::Binary;
                                cb.elem_bytes = (std::max<size_t>)(1u, (std::min<size_t>)(static_cast<size_t>(csize), 4096u));
                            } else if (is_unicode_sql_type(dtype)) {
                                cb.kind       = BindKind::WChar;
                                const size_t wchars = (std::max<size_t>)(1u, (std::min<size_t>)(static_cast<size_t>(csize), 4096u)) + 1u;
                                cb.elem_bytes = wchars * sizeof(SQLWCHAR);
                            } else {
                                cb.kind       = BindKind::Char;
                                const size_t chars = (std::max<size_t>)(64u, (std::min<size_t>)(static_cast<size_t>(csize) + 8u, 4097u));
                                cb.elem_bytes = chars;
                            }
                            row_stride_bytes += cb.elem_bytes;
                        }
                        static constexpr size_t kMaxBindBufferBytes = 16ull * 1024 * 1024;
                        const SQLULEN fetch_rows = (row_stride_bytes > 0)
                            ? static_cast<SQLULEN>(std::clamp<size_t>(kMaxBindBufferBytes / row_stride_bytes, 16u, kBlockFetchRows))
                            : kBlockFetchRows;

                        bool bind_ok = true;
                        SQLULEN rows_fetched = 0;
                        SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_BIND_TYPE, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_BIND_BY_COLUMN)), 0);
                        SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(fetch_rows)), 0);
                        SQLSetStmtAttrW(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0);

                        for (uint32_t c = 0; c < stored_cols; ++c) {
                            BoundColBuffer& cb = cols[c];
                            cb.indicators.assign(fetch_rows, 0);
                            cb.data.assign(fetch_rows * cb.elem_bytes, 0);
                            const SQLSMALLINT target_type =
                                (cb.kind == BindKind::Binary) ? SQL_C_BINARY
                                : (cb.kind == BindKind::WChar) ? SQL_C_WCHAR
                                                               : SQL_C_CHAR;
                            const SQLRETURN brc = SQLBindCol(
                                hstmt,
                                static_cast<SQLUSMALLINT>(c + 1u),
                                target_type,
                                cb.data.data(),
                                static_cast<SQLLEN>(cb.elem_bytes),
                                cb.indicators.data());
                            if (!SQL_SUCCEEDED(brc)) {
                                bind_ok = false;
                                break;
                            }
                        }

                        if (bind_ok) {
                            for (;;) {
                                if (cancel.is_canceled()) {
                                    writer->finish();
                                    SQLFreeStmt(hstmt, SQL_UNBIND);
                                    SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(1), 0);
                                    out.canceled.store(true, std::memory_order_release);
                                    return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                                }
                                rows_fetched = 0;
                                const SQLRETURN frc = SQLFetch(hstmt);
                                if (frc == SQL_NO_DATA) {
                                    break;
                                }
                                if (frc == SQL_SUCCESS_WITH_INFO) {
                                    drain_odbc_notices(hstmt, out, sink);
                                }
                                if (!SQL_SUCCEEDED(frc)) {
                                    writer->finish();
                                    SQLFreeStmt(hstmt, SQL_UNBIND);
                                    SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(1), 0);
                                    DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
                                    if (cancel.is_canceled()) {
                                        err.is_canceled = true;
                                        out.canceled.store(true, std::memory_order_release);
                                    } else {
                                        out.has_error.store(true, std::memory_order_release);
                                    }
                                    return std::unexpected(std::move(err));
                                }

                                const SQLULEN n_rows = (std::min)(rows_fetched, fetch_rows);
                                for (SQLULEN r = 0; r < n_rows; ++r) {
                                    if (writer->arena_limit_hit() || !writer->begin_row()) {
                                        break;
                                    }
                                    for (uint32_t c = 0; c < stored_cols; ++c) {
                                        const BoundColBuffer& cb = cols[c];
                                        const SQLLEN ind = cb.indicators[r];
                                        if (ind == SQL_NULL_DATA) {
                                            writer->write_null(c);
                                            continue;
                                        }
                                        const uint8_t* ptr = cb.data.data() + static_cast<size_t>(r) * cb.elem_bytes;
                                        if (cb.kind == BindKind::Binary) {
                                            const size_t nbytes = (ind >= 0) ? (std::min<size_t>)(static_cast<size_t>(ind), cb.elem_bytes) : 0u;
                                            writer->write_binary_hex(c, ptr, nbytes, "0x");
                                        } else if (cb.kind == BindKind::WChar) {
                                            const size_t max_w = (cb.elem_bytes / sizeof(SQLWCHAR)) - 1u;
                                            const size_t wlen = (ind >= 0)
                                                ? (std::min<size_t>)(static_cast<size_t>(ind) / sizeof(SQLWCHAR), max_w)
                                                : 0u;
                                            writer->write_utf16(c, reinterpret_cast<const char16_t*>(ptr), wlen);
                                        } else {
                                            const size_t max_c = cb.elem_bytes - 1u;
                                            const size_t clen = (ind >= 0)
                                                ? (std::min<size_t>)(static_cast<size_t>(ind), max_c)
                                                : 0u;
                                            writer->write_utf8(c, reinterpret_cast<const char*>(ptr), clen, false);
                                        }
                                    }
                                    (void)writer->end_row(false);
                                }
                                (void)writer->flush();
                                (void)sink.on_rows_published(out.total_rows_read.load(std::memory_order_acquire));
                            }
                            SQLFreeStmt(hstmt, SQL_UNBIND);
                            SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(1), 0);
                            SQLSetStmtAttrW(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0);
                            writer->finish();
                            sink.notify_if_unscheduled();
                        } else {
                            SQLFreeStmt(hstmt, SQL_UNBIND);
                            SQLSetStmtAttrW(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(1), 0);
                            SQLSetStmtAttrW(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0);
                            has_lob = true;
                        }
                    }

                    if (has_lob || opts.discard_results || writer == nullptr || stored_cols == 0) {
                        // LOB / Discard Path: Row-by-row SQLFetch + SQLGetData into reusable scratch buffers
                        std::vector<uint8_t>  bin_scratch;
                        std::vector<char16_t> w_scratch;
                        auto last_flush_tp = std::chrono::steady_clock::now();

                        for (;;) {
                            if (cancel.is_canceled()) {
                                if (writer != nullptr) writer->finish();
                                out.canceled.store(true, std::memory_order_release);
                                return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                            }
                            const SQLRETURN frc = SQLFetch(hstmt);
                            if (frc == SQL_NO_DATA) {
                                break;
                            }
                            if (frc == SQL_SUCCESS_WITH_INFO) {
                                drain_odbc_notices(hstmt, out, sink);
                            }
                            if (!SQL_SUCCEEDED(frc)) {
                                if (writer != nullptr) writer->finish();
                                DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
                                if (cancel.is_canceled()) {
                                    err.is_canceled = true;
                                    out.canceled.store(true, std::memory_order_release);
                                } else {
                                    out.has_error.store(true, std::memory_order_release);
                                }
                                return std::unexpected(std::move(err));
                            }

                            if (opts.discard_results) {
                                const uint64_t total = out.total_rows_read.fetch_add(1, std::memory_order_acq_rel) + 1;
                                (void)sink.on_rows_published(total);
                                continue;
                            }

                            if (writer == nullptr || writer->arena_limit_hit() || !writer->begin_row()) {
                                continue;
                            }

                            for (uint32_t c = 0; c < stored_cols; ++c) {
                                if (cancel.is_canceled()) {
                                    writer->finish();
                                    out.canceled.store(true, std::memory_order_release);
                                    return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                                }
                                if (is_binary_sql_type(sql_types[c])) {
                                    stream_lob_binary_cell(
                                        hstmt,
                                        static_cast<SQLUSMALLINT>(c + 1u),
                                        c,
                                        *writer,
                                        bin_scratch,
                                        opts.max_cell_bytes,
                                        col_is_hierarchyid[c]);
                                } else {
                                    stream_lob_wchar_cell(hstmt, static_cast<SQLUSMALLINT>(c + 1u), c, *writer, w_scratch, opts.max_cell_bytes);
                                }
                            }
                            const uint64_t rs_rows = writer->staged_and_published_rows() + 1;
                            const auto now = std::chrono::steady_clock::now();
                            const bool milestone_flush =
                                (rs_rows == 1 || rs_rows == 30 || rs_rows == 100) ||
                                (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_tp).count() >= 100);
                            (void)writer->end_row(milestone_flush);
                            if (writer->staged_row_count() == 0) {
                                last_flush_tp = now;
                                (void)sink.on_rows_published(out.total_rows_read.load(std::memory_order_acquire));
                            }
                        }
                        if (writer != nullptr) {
                            writer->finish();
                        } else if (rs != nullptr) {
                            rs->complete.store(true, std::memory_order_release);
                        }
                        sink.notify_if_unscheduled();
                    }
                } else {
                    SQLLEN affected = -1;
                    if (SQL_SUCCEEDED(SQLRowCount(hstmt, &affected)) && affected >= 0) {
                        out.add_rows_affected(static_cast<int64_t>(affected));
                    }
                }

                if (cancel.is_canceled()) {
                    out.canceled.store(true, std::memory_order_release);
                    return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                }

                {
                    std::lock_guard<std::mutex> lk(sess->cancel_mu);
                    sess->active_stmt = hstmt;
                }
                const SQLRETURN more_rc = SQLMoreResults(hstmt);
                {
                    std::lock_guard<std::mutex> lk(sess->cancel_mu);
                    if (sess->active_stmt == hstmt) {
                        sess->active_stmt = SQL_NULL_HSTMT;
                    }
                }
                if (more_rc == SQL_NO_DATA) {
                    break;
                }
                if (more_rc == SQL_SUCCESS_WITH_INFO) {
                    drain_odbc_notices(hstmt, out, sink);
                    continue;
                }
                if (!SQL_SUCCEEDED(more_rc)) {
                    DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
                    if (cancel.is_canceled()) {
                        err.is_canceled = true;
                        out.canceled.store(true, std::memory_order_release);
                    } else {
                        out.has_error.store(true, std::memory_order_release);
                    }
                    return std::unexpected(std::move(err));
                }
            }
        }
    }

    return {};
}

std::expected<void, DbError> MsSqlDriver::parse_only(
    std::string_view sql,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Parse canceled.", .is_canceled = true});
    }
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return std::unexpected(DbError{.message = "Not connected to SQL Server."});
    }

    const std::vector<TSqlBatch> batches = split_tsql_batches(sql);
    if (batches.empty()) {
        return {};
    }

    auto exec_control_stmt = [&](std::string_view cmd) -> std::expected<void, DbError> {
        SQLHSTMT hstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
            return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
        }
        const SqlWString wcmd = to_sqlw(cmd);
        const SQLRETURN rc = SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wcmd.c_str()), static_cast<SQLINTEGER>(wcmd.size()));
        if (!SQL_SUCCEEDED(rc)) {
            DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            return std::unexpected(std::move(err));
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return {};
    };

    if (auto on_res = exec_control_stmt("SET PARSEONLY ON"); !on_res) {
        return std::unexpected(on_res.error());
    }
    struct ParseOffGuard final {
        const decltype(exec_control_stmt)& fn;
        ~ParseOffGuard() {
            (void)fn("SET PARSEONLY OFF");
        }
    } parse_off_guard{exec_control_stmt};

    for (const TSqlBatch& batch : batches) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Parse canceled.", .is_canceled = true});
        }
        SQLHSTMT hstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
            return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
        }
        {
            std::lock_guard<std::mutex> lk(sess->cancel_mu);
            sess->active_stmt = hstmt;
        }

        const SqlWString wsql = to_sqlw(batch.sql);
        const SQLRETURN rc = SQLExecDirectW(
            hstmt,
            const_cast<SQLWCHAR*>(wsql.c_str()),
            static_cast<SQLINTEGER>(wsql.size()));

        {
            std::lock_guard<std::mutex> lk(sess->cancel_mu);
            sess->active_stmt = SQL_NULL_HSTMT;
        }

        if (!SQL_SUCCEEDED(rc)) {
            DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
            if (err.position <= 0) {
                err.position = static_cast<int32_t>(batch.start_line);
            } else {
                err.position += static_cast<int32_t>(batch.start_line - 1);
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            return std::unexpected(std::move(err));
        }
        while (SQLMoreResults(hstmt) != SQL_NO_DATA) {
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    }

    return {};
}

std::expected<std::vector<core::RawExecutionPlan>, DbError> MsSqlDriver::explain_plan(
    std::string_view sql,
    bool analyze,
    uint32_t timeout_sec,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Execution plan canceled.", .is_canceled = true});
    }
    const auto sess = acquire_session();
    if (!sess || !sess->connected) {
        return std::unexpected(DbError{.message = "Not connected to SQL Server."});
    }

    const std::vector<TSqlBatch> batches = split_tsql_batches(sql);
    if (batches.empty()) {
        return std::unexpected(DbError{.message = "Enter a T-SQL statement to explain."});
    }

    auto exec_control_stmt = [&](std::string_view cmd) -> std::expected<void, DbError> {
        SQLHSTMT hstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
            return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
        }
        const SqlWString wcmd = to_sqlw(cmd);
        const SQLRETURN rc = SQLExecDirectW(hstmt, const_cast<SQLWCHAR*>(wcmd.c_str()), static_cast<SQLINTEGER>(wcmd.size()));
        if (!SQL_SUCCEEDED(rc)) {
            DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            return std::unexpected(std::move(err));
        }
        while (SQLMoreResults(hstmt) != SQL_NO_DATA) {}
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        return {};
    };

    const char* on_cmd  = analyze ? "SET STATISTICS XML ON" : "SET SHOWPLAN_XML ON";
    const char* off_cmd = analyze ? "SET STATISTICS XML OFF" : "SET SHOWPLAN_XML OFF";
    if (auto on_res = exec_control_stmt(on_cmd); !on_res) {
        return std::unexpected(on_res.error());
    }
    struct PlanOffGuard final {
        const decltype(exec_control_stmt)& fn;
        const char* cmd;
        ~PlanOffGuard() {
            (void)fn(cmd);
        }
    } off_guard{exec_control_stmt, off_cmd};

    std::vector<core::RawExecutionPlan> plans;
    for (const TSqlBatch& batch : batches) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Execution plan canceled.", .is_canceled = true});
        }
        SQLHSTMT hstmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, sess->hdbc, &hstmt))) {
            return std::unexpected(extract_diag(SQL_HANDLE_DBC, sess->hdbc));
        }
        if (timeout_sec > 0) {
            SQLSetStmtAttrW(
                hstmt,
                SQL_ATTR_QUERY_TIMEOUT,
                reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(timeout_sec)),
                0);
        }
        {
            std::lock_guard<std::mutex> lk(sess->cancel_mu);
            sess->active_stmt = hstmt;
        }
        const SqlWString wsql = to_sqlw(batch.sql);
        const SQLRETURN rc = SQLExecDirectW(
            hstmt,
            const_cast<SQLWCHAR*>(wsql.c_str()),
            static_cast<SQLINTEGER>(wsql.size()));
        {
            std::lock_guard<std::mutex> lk(sess->cancel_mu);
            sess->active_stmt = SQL_NULL_HSTMT;
        }
        if (!SQL_SUCCEEDED(rc)) {
            DbError err = extract_diag(SQL_HANDLE_STMT, hstmt);
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            if (cancel.is_canceled()) err.is_canceled = true;
            return std::unexpected(std::move(err));
        }

        for (;;) {
            SQLSMALLINT num_cols = 0;
            if (SQL_SUCCEEDED(SQLNumResultCols(hstmt, &num_cols)) && num_cols > 0) {
                bool is_xml_plan = false;
                if (num_cols == 1) {
                    std::array<SQLWCHAR, 256> cname{};
                    SQLSMALLINT clen = 0;
                    SQLSMALLINT dt = 0;
                    SQLULEN cs = 0;
                    SQLSMALLINT sc = 0;
                    SQLSMALLINT nul = 0;
                    if (SQL_SUCCEEDED(SQLDescribeColW(hstmt, 1, cname.data(), static_cast<SQLSMALLINT>(cname.size()), &clen, &dt, &cs, &sc, &nul))) {
                        const std::string name = from_sqlw(cname.data(), clen > 0 ? static_cast<size_t>(clen) : 0u);
                        is_xml_plan = !analyze || is_showplan_xml_column(name);
                    }
                }
                while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
                    if (is_xml_plan) {
                        std::string xml = read_full_wchar_column(hstmt, 1, core::kMaxExecutionPlanBytes);
                        if (!xml.empty() && xml.find("ShowPlanXML") != std::string::npos) {
                            plans.push_back(core::RawExecutionPlan{
                                .source_kind = core::PlanSourceKind::SqlServerXml,
                                .raw_text    = std::move(xml),
                                .analyzed    = analyze,
                                .query_text  = batch.sql,
                            });
                        }
                    }
                }
            }
            const SQLRETURN mrc = SQLMoreResults(hstmt);
            if (mrc == SQL_NO_DATA || !SQL_SUCCEEDED(mrc)) {
                break;
            }
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    }

    if (plans.empty()) {
        return std::unexpected(DbError{.message = "SQL Server returned no ShowPlan XML for the submitted SQL."});
    }
    return plans;
}

} // namespace capybaradb::drivers
