#pragma once

#include "core/plan/execution_plan.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/cancel_token.hpp"
#include "core/threading/stream_sink.hpp"
#include "drivers/common/db_types.hpp"

#include <expected>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

/// Coarse-grained database driver abstraction.
/// Individual engine drivers execute hot-path loops directly without virtual calls per row.
class DbDriver {
public:
    virtual ~DbDriver() = default;

    [[nodiscard]] virtual std::expected<void, DbError> connect(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel) = 0;

    virtual void disconnect() noexcept = 0;

    [[nodiscard]] virtual bool is_connected() const noexcept = 0;
    [[nodiscard]] virtual SessionInfo session_info() const = 0;

    [[nodiscard]] virtual std::expected<ConnectionTestResult, DbError> test_connection(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<void, DbError> switch_database(
        std::string_view database,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<SessionLossRisk, DbError> inspect_loss_risk(
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<void, DbError> commit_transaction(
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<void, DbError> execute_stream(
        std::string_view sql,
        const ExecOptions& opts,
        core::QueryExecutionResult& out,
        core::StreamPublishSink& sink,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<void, DbError> parse_only(
        std::string_view sql,
        core::CancelToken& cancel) = 0;

    /// Computes estimated (analyze == false) or analyzed/actual (analyze == true) execution plan(s).
    [[nodiscard]] virtual std::expected<std::vector<core::RawExecutionPlan>, DbError> explain_plan(
        std::string_view /*sql*/,
        bool /*analyze*/,
        uint32_t /*timeout_sec*/,
        core::CancelToken& /*cancel*/) {
        return std::unexpected(DbError{.message = "Execution plans are not implemented by this driver."});
    }

    /// Thread-safe: may be called from the UI thread while a command is in flight.
    virtual void cancel_active() noexcept = 0;
};

} // namespace capybaradb::drivers
