#pragma once

#include "core/config/query_options.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace capybaradb::core {

enum class PlanSourceKind : uint8_t {
    PostgreSqlJson = 0,
    SqlServerXml   = 1,
};

struct RawExecutionPlan {
    PlanSourceKind source_kind = PlanSourceKind::PostgreSqlJson;
    std::string    raw_text;
    bool           analyzed    = false;
    std::string    query_text;
};

enum class ExecutionPlanIcon : uint8_t {
    Statement = 0,
    TableScan,
    IndexScan,
    BitmapScan,
    Join,
    Sort,
    Aggregate,
    Limit,
    Materialize,
    Append,
    Gather,
    Modify,
    Subquery,
    Other,
};

struct PlanIndexSuggestion {
    std::string               schema;
    std::string               table;
    std::vector<std::string>  key_columns;
    std::vector<std::string>  include_columns;
    std::optional<double>     impact_percent;
    std::string               header_line;
    std::string               commented_script;
};

struct ExecutionPlanOperator {
    std::string                                       title;
    std::string                                       subtitle;
    std::string                                       object_name;
    ExecutionPlanIcon                                 icon           = ExecutionPlanIcon::Other;
    double                                            cost_percent   = 0.0;
    std::optional<double>                             seconds;
    std::optional<double>                             actual_rows;
    double                                            estimated_rows = 0.0;
    bool                                              is_statement   = false;
    std::string                                       description;
    std::vector<std::pair<std::string, std::string>>  summary;
    std::vector<std::pair<std::string, std::string>>  details;
    std::vector<std::string>                          warnings;
    std::vector<std::pair<std::string, std::string>>  properties;
    std::vector<ExecutionPlanOperator>                children;

    [[nodiscard]] double flow_rows() const noexcept {
        return actual_rows.value_or(estimated_rows);
    }

    [[nodiscard]] std::vector<std::string> display_lines() const;
    [[nodiscard]] std::optional<std::string_view> property_value(std::string_view key) const noexcept;
};

struct ExecutionPlanStatement {
    int32_t                           number        = 1;
    std::string                       query_text;
    double                            batch_percent = 100.0;
    ExecutionPlanOperator             root;
    RawExecutionPlan                  source;
    std::vector<PlanIndexSuggestion>  missing_indexes;

    [[nodiscard]] std::string header() const;
};

enum class ExecutionPlanComparison : uint8_t {
    Contains    = 0,
    Equal       = 1,
    GreaterThan = 2,
    LessThan    = 3,
};

inline constexpr size_t kMaxExecutionPlanBytes = 4ull * 1024u * 1024u;
inline constexpr size_t kMaxExecutionPlanNodes = 4096u;
inline constexpr size_t kMaxExecutionPlanDepth = 64u;

/// Builds graphical ExecutionPlanStatement objects from PostgreSQL JSON and/or SQL Server XML plans.
[[nodiscard]] std::expected<std::vector<ExecutionPlanStatement>, std::string> parse_execution_plans(
    const std::vector<RawExecutionPlan>& plans,
    std::string_view fallback_query_text = {},
    const ExecutionPlanWarningOptions& warnings = {});

/// Auto-detects PostgreSQL JSON or SQL Server ShowPlan XML from raw file content.
[[nodiscard]] std::expected<std::vector<ExecutionPlanStatement>, std::string> parse_execution_plan_auto(
    std::string_view content,
    const ExecutionPlanWarningOptions& warnings = {});

/// Saves raw plan source atomically via sibling `.partial` + rename.
[[nodiscard]] std::expected<void, std::string> save_raw_execution_plan(
    const RawExecutionPlan& plan,
    const std::filesystem::path& destination);

/// Loads and parses a saved `.json` or `.sqlplan`/`.xml` execution plan file.
[[nodiscard]] std::expected<std::vector<ExecutionPlanStatement>, std::string> load_execution_plan_file(
    const std::filesystem::path& source_path,
    const ExecutionPlanWarningOptions& warnings = {});

[[nodiscard]] std::vector<const ExecutionPlanOperator*> collect_plan_operators(
    const std::vector<ExecutionPlanStatement>& statements);

[[nodiscard]] std::vector<std::string> collect_plan_property_names(
    const std::vector<const ExecutionPlanOperator*>& operators);

[[nodiscard]] bool plan_operator_matches(
    const ExecutionPlanOperator& op,
    std::string_view property,
    ExecutionPlanComparison cmp,
    std::string_view value);

[[nodiscard]] const ExecutionPlanOperator* find_next_plan_operator(
    const std::vector<const ExecutionPlanOperator*>& operators,
    const ExecutionPlanOperator* current,
    std::string_view property,
    ExecutionPlanComparison cmp,
    std::string_view value,
    bool forward = true);

} // namespace capybaradb::core
