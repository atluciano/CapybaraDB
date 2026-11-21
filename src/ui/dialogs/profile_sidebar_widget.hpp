#pragma once

#include "core/config/connection_profile.hpp"

#include <QWidget>
#include <span>
#include <string>

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace capybaradb::ui {

class ProfileSidebarWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProfileSidebarWidget(QWidget* parent = nullptr);

    void set_profiles(
        std::span<const core::ConnectionProfile> profiles,
        const std::string& selected_id = {});

    void select_profile_by_id(const std::string& profile_id);
    void clear_selection();

    [[nodiscard]] QString selected_profile_id() const;
    [[nodiscard]] QString selected_group_name() const;
    [[nodiscard]] int visible_profile_count() const;

    void set_controls_enabled(bool enabled, bool has_selected_profile);

    // Accessors for testing and inspection
    [[nodiscard]] QPushButton* new_button() const noexcept { return new_btn_; }
    [[nodiscard]] QLineEdit* filter_edit() const noexcept { return filter_edit_; }
    [[nodiscard]] QTreeWidget* tree_widget() const noexcept { return tree_; }
    [[nodiscard]] QPushButton* duplicate_button() const noexcept { return duplicate_btn_; }
    [[nodiscard]] QPushButton* delete_button() const noexcept { return delete_btn_; }

signals:
    void profile_selected(const QString& profile_id);
    void profile_activated(const QString& profile_id);
    void new_connection_requested();
    void duplicate_profile_requested();
    void delete_profile_requested();

private:
    void apply_filter(const QString& filter_text);
    void on_tree_selection_changed();
    void on_item_double_clicked(QTreeWidgetItem* item, int column);

    bool         suppress_signals_ = false;
    QPushButton* new_btn_          = nullptr;
    QLineEdit*   filter_edit_      = nullptr;
    QTreeWidget* tree_             = nullptr;
    QPushButton* duplicate_btn_    = nullptr;
    QPushButton* delete_btn_       = nullptr;
};

} // namespace capybaradb::ui
