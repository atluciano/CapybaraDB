#pragma once

#include "drivers/common/db_driver.hpp"

struct sqlite3;

namespace capybaradb::drivers {

class SqliteDriver final : public DbDriver {
public:
    SqliteDriver() = default;
    ~SqliteDriver() override;

    SqliteDriver(const SqliteDriver&) = delete;
    SqliteDriver& operator=(const SqliteDriver&) = delete;

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

private:
    sqlite3*    db_ = nullptr;
    std::string db_path_;
    std::string server_version_;
};

} // namespace capybaradb::drivers
