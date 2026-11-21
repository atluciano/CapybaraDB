#pragma once

#include <QIcon>
#include <QPixmap>
#include <cstdint>
#include <span>

namespace capybaradb::ui {

enum class IconId : uint8_t {
    // Main Toolbar
    Open = 0,
    Save,
    NewQuery,
    Execute,
    ExecuteDisabled,
    Cancel,
    CancelDisabled,
    PlanEstimated,
    PlanActual,
    ResultsText,
    ResultsGrid,
    ResultsFile,
    QueryOptions,

    // Object Explorer
    Plug,
    PlugOff,
    Stop,
    StopIdle,
    Filter,
    Refresh,
    Folder,
    Server,
    Database,
    Schema,
    Table,
    Partition,
    View,
    MaterializedView,
    Sequence,
    Routine,
    Column,
    Index,
    Role,
    Enum,
    Domain,
    Diagram,
    TreeExpanderClosed,
    TreeExpanderOpen,

    // OutputTabs, Query Toolbar, and Query Status Bar
    TabResults,
    TabMessages,
    TabPlan,
    TabSession,
    TabStats,
    ExportArrow,
    StatusSuccess,
    StatusRunning,
    StatusError,
    StatusIdle,
    GoToError,
    DesignQuery,

    // Engine Badges & Dialog Actions
    EnginePostgres,
    EngineMssql,
    EngineSqlite,
    BrowseFile,
};

inline constexpr size_t kIconCount = static_cast<size_t>(IconId::BrowseFile) + 1u;

/// Returns a cached DPR-aware QIcon from a flat direct-indexed array (zero std::unordered_map lookups).
[[nodiscard]] const QIcon& get_icon(IconId id);

/// Renders a sharp pixmap at logical base_px scaled by devicePixelRatio.
[[nodiscard]] QPixmap render_icon_pixmap(IconId id, int base_px = 16, qreal dpr = 1.0);

/// Returns all defined IconId values for verification and diagnostics.
[[nodiscard]] std::span<const IconId> all_icon_ids() noexcept;

/// Returns the Qt resource path for an icon ID.
[[nodiscard]] const char* icon_resource_path(IconId id) noexcept;

} // namespace capybaradb::ui
