#pragma once

#include "drivers/common/catalog_provider.hpp"

namespace capybaradb::drivers {

class SqliteCatalogProvider final : public CatalogProvider {
public:
    [[nodiscard]] std::expected<std::vector<CatalogNodeData>, DbError> list_children(
        const ConnectionConfig& base_cfg,
        const CatalogNodeData& parent,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<std::vector<std::string>, DbError> list_databases(
        const ConnectionConfig& base_cfg,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::expected<std::vector<std::string>, DbError> read_table_columns(
        const ConnectionConfig& base_cfg,
        std::string_view database,
        std::string_view schema,
        std::string_view table,
        core::CancelToken& cancel) override;

    [[nodiscard]] std::string generate_select_top_sql(
        std::string_view schema,
        std::string_view object,
        std::span<const std::string> columns,
        uint32_t top_n) const override;

    [[nodiscard]] std::expected<core::SqlCompletionCatalog, DbError> load_completion_catalog(
        const ConnectionConfig& base_cfg,
        core::CancelToken& cancel) override;
};

} // namespace capybaradb::drivers
