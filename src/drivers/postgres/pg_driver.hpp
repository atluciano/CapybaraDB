#pragma once

#include "drivers/common/db_driver.hpp"

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

struct pg_conn;
struct pg_result;
struct pg_cancel;

namespace capybaradb::drivers {

struct PgConnDeleter {
    void operator()(pg_conn* conn) const noexcept;
};

struct PgResultDeleter {
    void operator()(pg_result* res) const noexcept;
};

using PgConnPtr   = std::unique_ptr<pg_conn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<pg_result, PgResultDeleter>;

class PgDriver final : public DbDriver {
public:
    PgDriver() = default;
    ~PgDriver() override;

    PgDriver(const PgDriver&) = delete;
    PgDriver& operator=(const PgDriver&) = delete;

    [[nodiscard]] std::expected<void, DbError> connect(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel) override;

    void disconnect() noexcept override;

    [[nodiscard]] bool is_connected() const noexcept override;
    [[nodiscard]] SessionInfo session_info() const override;

    [[nodiscard]] std::expected<ConnectionTestResult, DbError> test_connection(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<void, DbError> switch_database(
        std::string_view database,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<SessionLossRisk, DbError> inspect_loss_risk(
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<void, DbError> commit_transaction(
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<void, DbError> execute_stream(
        std::string_view sql,
        const ExecOptions& opts,
        core::QueryExecutionResult& out,
        core::StreamPublishSink& sink,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<void, DbError> parse_only(
        std::string_view sql,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<std::vector<core::RawExecutionPlan>, DbError> explain_plan(
        std::string_view sql,
        bool analyze,
        uint32_t timeout_sec,
        core::CancelToken& cancel) override;

    void cancel_active() noexcept override;

    /// Shared helpers for PostgreSQL operations (e.g. PgCatalogProvider).
    [[nodiscard]] static std::expected<PgConnPtr, DbError> open_connection(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel);

    [[nodiscard]] static std::expected<PgResultPtr, DbError> exec_params_cancelable(
        pg_conn* conn,
        const char* sql,
        std::span<const char* const> param_values,
        core::CancelToken& cancel,
        uint32_t timeout_sec = 15);

    [[nodiscard]] static DbError error_from_result(const pg_result* res, const pg_conn* conn);

private:
    static std::expected<SessionInfo, DbError> query_session_info(
        pg_conn* conn,
        const ConnectionConfig& cfg,
        core::CancelToken& cancel);

    static std::unordered_map<uint32_t, std::string> load_extension_type_map(
        pg_conn* conn,
        core::CancelToken& cancel);

    mutable std::mutex                      conn_mu_;
    PgConnPtr                               conn_;
    ConnectionConfig                        current_cfg_;
    SessionInfo                             session_info_;
    std::unordered_map<uint32_t, std::string> custom_oid_names_;
    bool                                    plan_capture_armed_ = false;

    std::mutex cancel_mu_;
    pg_cancel* active_cancel_ = nullptr;
};

} // namespace capybaradb::drivers
