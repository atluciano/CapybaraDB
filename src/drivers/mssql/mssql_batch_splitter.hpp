#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

struct TSqlBatch {
    std::string sql;
    uint32_t    repeat_count = 1;
    uint32_t    start_line   = 1;
};

/// Lexical splitter for T-SQL batch separator directives (`GO [count]`),
/// ignoring occurrences inside `'...'`, `N'...'`, `[...]`, `"..."`, `--` comments,
/// and nested `/* ... */` comments.
[[nodiscard]] std::vector<TSqlBatch> split_tsql_batches(std::string_view script);

} // namespace capybaradb::drivers
