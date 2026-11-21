#include "drivers/sqlite/sqlite_driver.hpp"

#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {
namespace {

std::string sqlite_path_from_config(const ConnectionConfig& cfg) {
    if (!cfg.database.empty()) {
        return cfg.database;
    }
    if (!cfg.host.empty() && cfg.host != "localhost") {
        return cfg.host;
    }
    return ":memory:";
}

core::LogicalTypeKind classify_sqlite_type(const char* decl_type) {
    if (decl_type == nullptr) {
        return core::LogicalTypeKind::Unknown;
    }
    std::string s;
    for (const char* p = decl_type; *p != '\0'; ++p) {
        s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
    }
    if (s.find("INT") != std::string::npos) {
        return core::LogicalTypeKind::Int;
    }
    if (s.find("CHAR") != std::string::npos || s.find("CLOB") != std::string::npos || s.find("TEXT") != std::string::npos) {
        return core::LogicalTypeKind::Text;
    }
    if (s.find("BLOB") != std::string::npos) {
        return core::LogicalTypeKind::Binary;
    }
    if (s.find("REAL") != std::string::npos || s.find("FLOA") != std::string::npos || s.find("DOUB") != std::string::npos) {
        return core::LogicalTypeKind::Float;
    }
    if (s.find("NUMERIC") != std::string::npos || s.find("DECIMAL") != std::string::npos) {
        return core::LogicalTypeKind::Numeric;
    }
    if (s.find("BOOL") != std::string::npos) {
        return core::LogicalTypeKind::Bool;
    }
    if (s.find("DATE") != std::string::npos || s.find("TIME") != std::string::npos) {
        return core::LogicalTypeKind::Timestamp;
    }
    if (s.find("JSON") != std::string::npos) {
        return core::LogicalTypeKind::Json;
    }
    return core::LogicalTypeKind::Text;
}

} // namespace

SqliteDriver::~SqliteDriver() {
    disconnect();
}

std::expected<void, DbError> SqliteDriver::connect(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    disconnect();

    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }

    const std::string path = sqlite_path_from_config(cfg);
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(
        path.c_str(),
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI,
        nullptr);

    if (rc != SQLITE_OK) {
        std::string msg = db != nullptr ? sqlite3_errmsg(db) : "Failed to open SQLite database.";
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        return std::unexpected(DbError{
            .message = std::move(msg),
            .native_code = rc,
        });
    }

    db_ = db;
    db_path_ = path;
    server_version_ = sqlite3_libversion();

    sqlite3_busy_timeout(db_, 5000);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

    return {};
}

void SqliteDriver::disconnect() noexcept {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    db_path_.clear();
    server_version_.clear();
}

bool SqliteDriver::is_connected() const noexcept {
    return db_ != nullptr;
}

SessionInfo SqliteDriver::session_info() const {
    SessionInfo info;
    info.engine         = EngineKind::Sqlite;
    info.host           = "localhost";
    info.port           = 0;
    info.server_version = server_version_.empty() ? std::string(sqlite3_libversion()) : server_version_;
    info.database       = db_path_;
    info.username       = "sqlite";
    info.session_id     = 1;
    return info;
}

std::expected<ConnectionTestResult, DbError> SqliteDriver::test_connection(
    const ConnectionConfig& cfg,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }

    const std::string path = sqlite_path_from_config(cfg);
    sqlite3* test_db = nullptr;
    const int rc = sqlite3_open_v2(
        path.c_str(),
        &test_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI,
        nullptr);

    if (rc != SQLITE_OK) {
        std::string msg = test_db != nullptr ? sqlite3_errmsg(test_db) : "Failed to open SQLite database.";
        if (test_db != nullptr) {
            sqlite3_close_v2(test_db);
        }
        return std::unexpected(DbError{
            .message = std::move(msg),
            .native_code = rc,
        });
    }

    std::string ver = sqlite3_libversion();
    sqlite3_close_v2(test_db);

    ConnectionTestResult res;
    res.database   = path;
    res.user       = "sqlite";
    res.version    = "SQLite " + ver;
    res.tls_active = false;
    return res;
}

std::expected<void, DbError> SqliteDriver::switch_database(
    std::string_view database,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }
    ConnectionConfig cfg;
    cfg.engine   = EngineKind::Sqlite;
    cfg.database = std::string(database);
    return connect(cfg, cancel);
}

std::expected<SessionLossRisk, DbError> SqliteDriver::inspect_loss_risk(
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }
    SessionLossRisk risk;
    if (db_ != nullptr) {
        // In SQLite, sqlite3_get_autocommit returns 0 when inside an open transaction
        risk.open_transaction = (sqlite3_get_autocommit(db_) == 0);
    }
    return risk;
}

std::expected<void, DbError> SqliteDriver::commit_transaction(
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }
    if (db_ == nullptr) {
        return std::unexpected(DbError{.message = "Database not connected."});
    }
    char* err_msg = nullptr;
    const int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = err_msg != nullptr ? err_msg : sqlite3_errmsg(db_);
        sqlite3_free(err_msg);
        return std::unexpected(DbError{.message = std::move(msg), .native_code = rc});
    }
    return {};
}

std::expected<void, DbError> SqliteDriver::execute_stream(
    std::string_view sql,
    const ExecOptions& opts,
    core::QueryExecutionResult& out,
    core::StreamPublishSink& sink,
    core::CancelToken& cancel) {
    if (db_ == nullptr) {
        return std::unexpected(DbError{.message = "Database not connected."});
    }
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
    }

    const char* tail = sql.data();
    const char* end  = sql.data() + sql.size();

    while (tail < end) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
        }

        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v2(
            db_,
            tail,
            static_cast<int>(end - tail),
            &stmt,
            &tail);

        if (rc != SQLITE_OK) {
            return std::unexpected(DbError{
                .message     = sqlite3_errmsg(db_),
                .native_code = rc,
            });
        }

        if (stmt == nullptr) {
            // Empty statement / comment / trailing whitespace
            continue;
        }

        const int col_count = sqlite3_column_count(stmt);
        if (col_count > 0) {
            std::vector<core::ColumnSpec> specs;
            specs.reserve(static_cast<size_t>(col_count));
            for (int c = 0; c < col_count; ++c) {
                const char* name = sqlite3_column_name(stmt, c);
                const char* decl = sqlite3_column_decltype(stmt, c);
                specs.push_back(core::ColumnSpec{
                    .name          = (name != nullptr) ? std::string_view{name} : std::string_view{},
                    .type_name     = (decl != nullptr) ? std::string_view{decl} : std::string_view{"ANY"},
                    .logical_type  = classify_sqlite_type(decl),
                    .may_hold_json = (decl != nullptr && std::string_view{decl}.find("JSON") != std::string_view::npos),
                });
            }

            core::ResultSetStorage* rs = out.create_result_set(specs);
            if (rs == nullptr) {
                sqlite3_finalize(stmt);
                return std::unexpected(DbError{.message = "Out of memory creating result set storage."});
            }

            core::ResultSetWriter writer(out, *rs, opts.max_arena_bytes, opts.max_cell_bytes);
            auto last_flush = std::chrono::steady_clock::now();

            while (true) {
                if (cancel.is_canceled()) {
                    writer.finish();
                    sqlite3_finalize(stmt);
                    return std::unexpected(DbError{.message = "Query canceled by user.", .is_canceled = true});
                }

                const int step_rc = sqlite3_step(stmt);
                if (step_rc == SQLITE_ROW) {
                    if (opts.discard_results) {
                        const uint64_t total = out.total_rows_read.fetch_add(1, std::memory_order_acq_rel) + 1;
                        (void)sink.on_rows_published(total);
                        continue;
                    }

                    if (writer.arena_limit_hit()) {
                        continue;
                    }

                    if (!writer.begin_row()) {
                        continue;
                    }

                    for (int c = 0; c < col_count; ++c) {
                        const int type = sqlite3_column_type(stmt, c);
                        switch (type) {
                        case SQLITE_NULL:
                            writer.write_null(static_cast<uint32_t>(c));
                            break;
                        case SQLITE_INTEGER: {
                            const sqlite3_int64 val = sqlite3_column_int64(stmt, c);
                            char num_buf[32];
                            const auto [ptr, ec] = std::to_chars(num_buf, num_buf + sizeof(num_buf), val);
                            writer.write_utf8(static_cast<uint32_t>(c), num_buf, static_cast<size_t>(ptr - num_buf));
                            break;
                        }
                        case SQLITE_FLOAT: {
                            const double val = sqlite3_column_double(stmt, c);
                            char flt_buf[64];
                            const int len = std::snprintf(flt_buf, sizeof(flt_buf), "%.15g", val);
                            writer.write_utf8(static_cast<uint32_t>(c), flt_buf, len > 0 ? static_cast<size_t>(len) : 0);
                            break;
                        }
                        case SQLITE_BLOB: {
                            const auto* b = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, c));
                            const int nbytes = sqlite3_column_bytes(stmt, c);
                            writer.write_binary_hex(static_cast<uint32_t>(c), b, static_cast<size_t>(nbytes > 0 ? nbytes : 0));
                            break;
                        }
                        case SQLITE_TEXT:
                        default: {
                            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                            const int nbytes = sqlite3_column_bytes(stmt, c);
                            writer.write_utf8(static_cast<uint32_t>(c), txt, static_cast<size_t>(nbytes > 0 ? nbytes : 0));
                            break;
                        }
                        }
                    }

                    const uint64_t total = writer.end_row();
                    out.total_rows_read.store(total, std::memory_order_release);

                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_flush >= std::chrono::milliseconds(100)) {
                        writer.flush();
                        sink.notify_if_unscheduled();
                        last_flush = now;
                    }
                } else if (step_rc == SQLITE_DONE) {
                    break;
                } else {
                    DbError err{
                        .message     = sqlite3_errmsg(db_),
                        .native_code = step_rc,
                    };
                    writer.finish();
                    sqlite3_finalize(stmt);
                    return std::unexpected(err);
                }
            }

            writer.finish();
            sink.notify_if_unscheduled();
            sqlite3_finalize(stmt);
        } else {
            // DDL / DML statement
            int step_rc = SQLITE_OK;
            while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                // Drain any unexpected rows
            }
            if (step_rc != SQLITE_DONE) {
                DbError err{
                    .message     = sqlite3_errmsg(db_),
                    .native_code = step_rc,
                };
                sqlite3_finalize(stmt);
                return std::unexpected(err);
            }
            const int changed = sqlite3_changes(db_);
            out.add_rows_affected(changed);
            sqlite3_finalize(stmt);
        }
    }

    return {};
}

std::expected<void, DbError> SqliteDriver::parse_only(
    std::string_view sql,
    core::CancelToken& cancel) {
    if (db_ == nullptr) {
        return std::unexpected(DbError{.message = "Database not connected."});
    }

    const char* tail = sql.data();
    const char* end  = sql.data() + sql.size();

    while (tail < end) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v2(
            db_,
            tail,
            static_cast<int>(end - tail),
            &stmt,
            &tail);
        if (rc != SQLITE_OK) {
            return std::unexpected(DbError{
                .message     = sqlite3_errmsg(db_),
                .native_code = rc,
            });
        }
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }

    return {};
}

std::expected<std::vector<core::RawExecutionPlan>, DbError> SqliteDriver::explain_plan(
    std::string_view sql,
    bool /*analyze*/,
    uint32_t /*timeout_sec*/,
    core::CancelToken& cancel) {
    if (db_ == nullptr) {
        return std::unexpected(DbError{.message = "Database not connected."});
    }

    std::string explain_sql = "EXPLAIN QUERY PLAN " + std::string(sql);
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, explain_sql.c_str(), static_cast<int>(explain_sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(DbError{
            .message     = sqlite3_errmsg(db_),
            .native_code = rc,
        });
    }

    std::string plan_text;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancel.is_canceled()) {
            sqlite3_finalize(stmt);
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        const int id        = sqlite3_column_int(stmt, 0);
        const int parent    = sqlite3_column_int(stmt, 1);
        const auto* detail  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        plan_text += "[" + std::to_string(id) + " -> " + std::to_string(parent) + "] " +
                     (detail != nullptr ? detail : "") + "\n";
    }
    sqlite3_finalize(stmt);

    core::RawExecutionPlan plan;
    plan.raw_text = std::move(plan_text);
    return std::vector<core::RawExecutionPlan>{std::move(plan)};
}

void SqliteDriver::cancel_active() noexcept {
    if (db_ != nullptr) {
        sqlite3_interrupt(db_);
    }
}

} // namespace capybaradb::drivers
