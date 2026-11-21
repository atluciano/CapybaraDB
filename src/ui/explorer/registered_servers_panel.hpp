#pragma once

#include "core/config/connection_profile.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "drivers/common/db_types.hpp"

#include <QWidget>
#include <filesystem>
#include <optional>
#include <vector>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace capybaradb::ui {

/// Dockable Registered Servers tool panel (View -> Registered Servers, Ctrl+Alt+G).
class RegisteredServersPanel final : public QWidget {
    Q_OBJECT

public:
    explicit RegisteredServersPanel(
        std::filesystem::path state_dir,
        core::CatalogWorkerPool* worker_pool,
        QWidget* parent = nullptr);

    void refresh();
    bool create_group(const QString& group_path);

    [[nodiscard]] QTreeWidget* tree_widget() const noexcept { return tree_; }
    [[nodiscard]] QLabel* feedback_label() const noexcept { return feedback_; }

signals:
    void connect_in_explorer_requested(const capybaradb::drivers::ConnectionConfig& config);
    void new_query_requested(const capybaradb::drivers::ConnectionConfig& config);
    void status_message(const QString& message);

private:
    void build_ui();
    void on_new_group();
    void on_new_registration();
    void on_context_menu(const QPoint& pos);
    void on_item_activated(QTreeWidgetItem* item);
    [[nodiscard]] QString selected_group_path() const;
    [[nodiscard]] std::optional<core::ConnectionProfile> profile_for_item(const QTreeWidgetItem* item) const;
    [[nodiscard]] std::optional<drivers::ConnectionConfig> config_from_profile(const core::ConnectionProfile& p);

    std::filesystem::path          state_dir_;
    core::CatalogWorkerPool*       worker_pool_ = nullptr;
    std::vector<core::ConnectionProfile> profiles_;

    QTreeWidget* tree_     = nullptr;
    QLabel*      feedback_ = nullptr;
};

} // namespace capybaradb::ui
