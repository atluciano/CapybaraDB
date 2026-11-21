#pragma once

#include "core/completion/sql_completion.hpp"
#include "core/threading/cancel_token.hpp"
#include "drivers/common/db_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

enum class CatalogFilterOperator : uint8_t {
    Contains       = 0,
    Equals         = 1,
    DoesNotContain = 2,
};

struct CatalogFilter {
    std::string           name;
    CatalogFilterOperator name_operator  = CatalogFilterOperator::Contains;
    std::string           owner;
    CatalogFilterOperator owner_operator = CatalogFilterOperator::Equals;

    [[nodiscard]] bool is_active() const noexcept {
        return !name.empty() || !owner.empty();
    }

    [[nodiscard]] bool matches(std::string_view item_name, std::string_view item_owner = {}) const noexcept {
        return test(item_name, name, name_operator) && test(item_owner, owner, owner_operator);
    }

private:
    [[nodiscard]] static bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool icontains_ascii(std::string_view haystack, std::string_view needle) noexcept {
        if (needle.empty()) {
            return true;
        }
        const auto it = std::search(
            haystack.begin(),
            haystack.end(),
            needle.begin(),
            needle.end(),
            [](char c1, char c2) {
                return std::tolower(static_cast<unsigned char>(c1)) ==
                       std::tolower(static_cast<unsigned char>(c2));
            });
        return it != haystack.end();
    }

    [[nodiscard]] static bool test(std::string_view actual, std::string_view expected, CatalogFilterOperator op) noexcept {
        if (expected.empty()) {
            return true;
        }
        switch (op) {
        case CatalogFilterOperator::Equals:
            return iequals_ascii(actual, expected);
        case CatalogFilterOperator::DoesNotContain:
            return !icontains_ascii(actual, expected);
        case CatalogFilterOperator::Contains:
        default:
            return icontains_ascii(actual, expected);
        }
    }
};

enum class CatalogKind : uint8_t {
    Server = 0,
    Databases,
    Database,
    DatabaseDiagrams,
    DatabaseDiagram,
    Schemas,
    Schema,
    Tables,
    Views,
    MaterializedViews,
    Sequences,
    Functions,
    Procedures,
    Relation,
    Routine,
    Columns,
    Column,
    Keys,
    Key,
    Constraints,
    Constraint,
    Triggers,
    Trigger,
    Indexes,
    Index,
    Roles,
    Role,
    Enums,
    Domains,
    DataType,
    Placeholder,
};

struct CatalogNodeData {
    CatalogKind kind = CatalogKind::Placeholder;
    std::string name;
    std::string database;
    std::string schema;
    uint64_t    oid = 0;
    std::string role_name;
    std::string owner;
    char        relation_type  = '\0'; // 'r', 'p', 'f', 'v', 'm', 'S'
    char        data_type_kind = '\0'; // 'e', 'd', 't'
    char        routine_kind   = '\0'; // 'f', 'p'
    bool        is_offline     = false;

    [[nodiscard]] static CatalogNodeData make_folder(
        CatalogKind folder_kind,
        std::string folder_name,
        std::string db = {},
        std::string sch = {},
        uint64_t obj_oid = 0) {
        CatalogNodeData n;
        n.kind     = folder_kind;
        n.name     = std::move(folder_name);
        n.database = std::move(db);
        n.schema   = std::move(sch);
        n.oid      = obj_oid;
        return n;
    }

    [[nodiscard]] constexpr bool can_expand() const noexcept {
        if (is_offline) {
            return false;
        }
        switch (kind) {
        case CatalogKind::Routine:
        case CatalogKind::Column:
        case CatalogKind::Index:
        case CatalogKind::Role:
        case CatalogKind::DataType:
        case CatalogKind::DatabaseDiagram:
        case CatalogKind::Key:
        case CatalogKind::Constraint:
        case CatalogKind::Trigger:
        case CatalogKind::Placeholder:
            return false;
        default:
            return true;
        }
    }

    [[nodiscard]] constexpr bool can_filter_by_owner() const noexcept {
        switch (kind) {
        case CatalogKind::Tables:
        case CatalogKind::Views:
        case CatalogKind::MaterializedViews:
        case CatalogKind::Functions:
        case CatalogKind::Procedures:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr bool can_filter() const noexcept {
        return can_filter_by_owner()
            || kind == CatalogKind::Roles
            || kind == CatalogKind::Indexes;
    }

    [[nodiscard]] std::string filter_name() const {
        if (!role_name.empty()) {
            return role_name;
        }
        if (kind == CatalogKind::Routine) {
            const size_t paren = name.find('(');
            if (paren != std::string::npos) {
                return name.substr(0, paren);
            }
        }
        return name;
    }
};

class CatalogProvider {
public:
    virtual ~CatalogProvider() = default;

    [[nodiscard]] virtual std::expected<std::vector<CatalogNodeData>, DbError> list_children(
        const ConnectionConfig& base_cfg,
        const CatalogNodeData& parent,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<std::vector<std::string>, DbError> list_databases(
        const ConnectionConfig& base_cfg,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::expected<std::vector<std::string>, DbError> read_table_columns(
        const ConnectionConfig& base_cfg,
        std::string_view database,
        std::string_view schema,
        std::string_view table,
        core::CancelToken& cancel) = 0;

    [[nodiscard]] virtual std::string generate_select_top_sql(
        std::string_view schema,
        std::string_view object,
        std::span<const std::string> columns,
        uint32_t top_n) const = 0;

    /// Loads schema/table/view/column/function/FK metadata for IntelliSense on a dedicated connection.
    [[nodiscard]] virtual std::expected<core::SqlCompletionCatalog, DbError> load_completion_catalog(
        const ConnectionConfig& /*base_cfg*/,
        core::CancelToken& /*cancel*/) {
        return core::SqlCompletionCatalog{};
    }
};

} // namespace capybaradb::drivers
