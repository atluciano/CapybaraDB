#include "ui/shell/icon_catalog.hpp"

#include <QGuiApplication>
#include <QPainter>
#include <QSvgRenderer>
#include <QtGlobal>
#include <array>
#include <cmath>

static void capybaradb_init_icon_resources() {
    Q_INIT_RESOURCE(resources);
}

namespace capybaradb::ui {
namespace {

constexpr std::array<IconId, kIconCount> kAllIconIds = {
    IconId::Open,
    IconId::Save,
    IconId::NewQuery,
    IconId::Execute,
    IconId::ExecuteDisabled,
    IconId::Cancel,
    IconId::CancelDisabled,
    IconId::PlanEstimated,
    IconId::PlanActual,
    IconId::ResultsText,
    IconId::ResultsGrid,
    IconId::ResultsFile,
    IconId::QueryOptions,
    IconId::Plug,
    IconId::PlugOff,
    IconId::Stop,
    IconId::StopIdle,
    IconId::Filter,
    IconId::Refresh,
    IconId::Folder,
    IconId::Server,
    IconId::Database,
    IconId::Schema,
    IconId::Table,
    IconId::Partition,
    IconId::View,
    IconId::MaterializedView,
    IconId::Sequence,
    IconId::Routine,
    IconId::Column,
    IconId::Index,
    IconId::Role,
    IconId::Enum,
    IconId::Domain,
    IconId::Diagram,
    IconId::TreeExpanderClosed,
    IconId::TreeExpanderOpen,
    IconId::TabResults,
    IconId::TabMessages,
    IconId::TabPlan,
    IconId::TabSession,
    IconId::TabStats,
    IconId::ExportArrow,
    IconId::StatusSuccess,
    IconId::StatusRunning,
    IconId::StatusError,
    IconId::StatusIdle,
    IconId::GoToError,
    IconId::DesignQuery,
    IconId::EnginePostgres,
    IconId::EngineMssql,
    IconId::EngineSqlite,
    IconId::BrowseFile,
};

QPixmap render_svg(const char* path, int base_px, qreal dpr) {
    QSvgRenderer renderer(QString::fromLatin1(path));
    if (!renderer.isValid()) {
        return {};
    }
    const int pixel_w = std::max(1, static_cast<int>(std::lround(base_px * dpr)));
    const int pixel_h = std::max(1, static_cast<int>(std::lround(base_px * dpr)));
    QPixmap pm(pixel_w, pixel_h);
    pm.fill(Qt::transparent);
    {
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, QRectF(0.0, 0.0, static_cast<qreal>(pixel_w), static_cast<qreal>(pixel_h)));
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

} // namespace

const char* icon_resource_path(IconId id) noexcept {
    switch (id) {
    case IconId::Open: return ":/icons/open.svg";
    case IconId::Save: return ":/icons/save.svg";
    case IconId::NewQuery: return ":/icons/new_query.svg";
    case IconId::Execute: return ":/icons/execute.svg";
    case IconId::ExecuteDisabled: return ":/icons/execute_disabled.svg";
    case IconId::Cancel: return ":/icons/cancel.svg";
    case IconId::CancelDisabled: return ":/icons/cancel_disabled.svg";
    case IconId::PlanEstimated: return ":/icons/plan_estimated.svg";
    case IconId::PlanActual: return ":/icons/plan_actual.svg";
    case IconId::ResultsText: return ":/icons/results_text.svg";
    case IconId::ResultsGrid: return ":/icons/results_grid.svg";
    case IconId::ResultsFile: return ":/icons/results_file.svg";
    case IconId::QueryOptions: return ":/icons/query_options.svg";
    case IconId::Plug: return ":/icons/plug.svg";
    case IconId::PlugOff: return ":/icons/plug_off.svg";
    case IconId::Stop: return ":/icons/stop.svg";
    case IconId::StopIdle: return ":/icons/stop_idle.svg";
    case IconId::Filter: return ":/icons/filter.svg";
    case IconId::Refresh: return ":/icons/refresh.svg";
    case IconId::Folder: return ":/icons/folder.svg";
    case IconId::Server: return ":/icons/server.svg";
    case IconId::Database: return ":/icons/database.svg";
    case IconId::Schema: return ":/icons/schema.svg";
    case IconId::Table: return ":/icons/table.svg";
    case IconId::Partition: return ":/icons/partition.svg";
    case IconId::View: return ":/icons/view.svg";
    case IconId::MaterializedView: return ":/icons/materialized_view.svg";
    case IconId::Sequence: return ":/icons/sequence.svg";
    case IconId::Routine: return ":/icons/routine.svg";
    case IconId::Column: return ":/icons/column.svg";
    case IconId::Index: return ":/icons/index.svg";
    case IconId::Role: return ":/icons/role.svg";
    case IconId::Enum: return ":/icons/enum.svg";
    case IconId::Domain: return ":/icons/domain.svg";
    case IconId::Diagram: return ":/icons/diagram.svg";
    case IconId::TreeExpanderClosed: return ":/icons/tree_expander_closed.svg";
    case IconId::TreeExpanderOpen: return ":/icons/tree_expander_open.svg";
    case IconId::TabResults: return ":/icons/tab_results.svg";
    case IconId::TabMessages: return ":/icons/tab_messages.svg";
    case IconId::TabPlan: return ":/icons/tab_plan.svg";
    case IconId::TabSession: return ":/icons/tab_session.svg";
    case IconId::TabStats: return ":/icons/tab_stats.svg";
    case IconId::ExportArrow: return ":/icons/export_arrow.svg";
    case IconId::StatusSuccess: return ":/icons/status_success.svg";
    case IconId::StatusRunning: return ":/icons/status_running.svg";
    case IconId::StatusError: return ":/icons/status_error.svg";
    case IconId::StatusIdle: return ":/icons/status_idle.svg";
    case IconId::GoToError: return ":/icons/go_to_error.svg";
    case IconId::DesignQuery: return ":/icons/design_query.svg";
    case IconId::EnginePostgres: return ":/icons/engine_postgres.svg";
    case IconId::EngineMssql: return ":/icons/engine_mssql.svg";
    case IconId::EngineSqlite: return ":/icons/engine_sqlite.svg";
    case IconId::BrowseFile: return ":/icons/browse_file.svg";
    }
    return ":/icons/folder.svg";
}

std::span<const IconId> all_icon_ids() noexcept {
    return kAllIconIds;
}

QPixmap render_icon_pixmap(IconId id, int base_px, qreal dpr) {
    capybaradb_init_icon_resources();
    return render_svg(icon_resource_path(id), base_px, dpr);
}

const QIcon& get_icon(IconId id) {
    capybaradb_init_icon_resources();
    static std::array<QIcon, kIconCount> cache{};
    const size_t idx = (static_cast<size_t>(id) < kIconCount)
        ? static_cast<size_t>(id)
        : static_cast<size_t>(IconId::Folder);
    if (!cache[idx].isNull()) {
        return cache[idx];
    }

    QIcon icon;
    const char* path = icon_resource_path(id);
    const qreal active_dpr = (qGuiApp != nullptr) ? std::clamp(qGuiApp->devicePixelRatio(), 1.0, 4.0) : 1.0;
    for (const int base_px : {14, 16}) {
        const QPixmap pm = render_svg(path, base_px, active_dpr);
        if (!pm.isNull()) {
            icon.addPixmap(pm);
        }
        if (std::abs(active_dpr - 1.0) > 0.05) {
            const QPixmap pm1 = render_svg(path, base_px, 1.0);
            if (!pm1.isNull()) {
                icon.addPixmap(pm1);
            }
        }
    }

    cache[idx] = std::move(icon);
    return cache[idx];
}

} // namespace capybaradb::ui
