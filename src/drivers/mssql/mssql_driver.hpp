#pragma once

#include "drivers/common/db_driver.hpp"
#include "drivers/common/driver_registry.hpp"

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

class MsSqlDriver final : public DbDriver {
public:
    MsSqlDriver() = default;
    ~MsSqlDriver() override;

    MsSqlDriver(const MsSqlDriver&) = delete;
    MsSqlDriver& operator=(const MsSqlDriver&) = delete;

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

    [[nodiscard]] static std::vector<std::string> list_odbc_drivers();
    [[nodiscard]] static std::string build_connection_string(
        const ConnectionConfig& cfg,
        std::string_view driver_name) {
        return build_mssql_connection_string(cfg, driver_name);
    }

    struct OdbcSession;

    [[nodiscard]] static std::expected<OdbcSession, DbError> open_odbc_session(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel);

private:
    static std::expected<SessionInfo, DbError> query_session_info(
        OdbcSession& sess,
        const ConnectionConfig& cfg,
        core::CancelToken& cancel);

    [[nodiscard]] std::shared_ptr<OdbcSession> acquire_session() const;

    mutable std::mutex           conn_mu_;
    std::shared_ptr<OdbcSession> session_;
    ConnectionConfig             current_cfg_;
    SessionInfo                  session_info_;
};

} // namespace capybaradb::drivers
