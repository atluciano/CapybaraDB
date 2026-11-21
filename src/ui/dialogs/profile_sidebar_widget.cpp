#include "ui/dialogs/profile_sidebar_widget.hpp"

#include "drivers/common/db_types.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"
#include "ui/shell/icon_catalog.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <unordered_map>

namespace capybaradb::ui {
namespace {

constexpr int kRoleItemKind  = Qt::UserRole;
constexpr int kRoleProfileId = Qt::UserRole + 1;
constexpr int kRoleGroupName = Qt::UserRole + 2;
constexpr int kRoleSummary   = Qt::UserRole + 3;

enum SidebarItemKind : int {
    kSidebarGroup   = 1,
    kSidebarProfile = 2,
};

IconId icon_for_engine(drivers::EngineKind engine) noexcept {
    switch (engine) {
    case drivers::EngineKind::PostgreSql: return IconId::EnginePostgres;
    case drivers::EngineKind::SqlServer:  return IconId::EngineMssql;
    case drivers::EngineKind::Sqlite:     return IconId::EngineSqlite;
    }
    return IconId::EnginePostgres;
}

QString format_profile_tooltip(const core::ConnectionProfile& p) {
    const auto eng = drivers::engine_kind_from_string(p.engine);
    const auto& desc = drivers::driver_ui_descriptor(eng);
    const QString eng_name = QString::fromUtf8(
        desc.display_name.data(),
        static_cast<qsizetype>(desc.display_name.size()));

    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        const QString db = p.database.empty()
            ? QStringLiteral(":memory:")
            : QString::fromStdString(p.database);
        return QStringLiteral("%1 · %2").arg(eng_name, db);
    }

    const QString user_str = (p.auth_mode == "Integrated" && p.username.empty())
        ? QStringLiteral("Integrated")
        : QString::fromStdString(p.username);
    return QStringLiteral("%1 · %2:%3 / %4 (%5)")
        .arg(eng_name, QString::fromStdString(p.host))
        .arg(p.port)
        .arg(QString::fromStdString(p.database), user_str);
}

} // namespace

ProfileSidebarWidget::ProfileSidebarWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("ProfileSidebarWidget"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(195);
    setMaximumWidth(235);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);

    new_btn_ = new QPushButton(QStringLiteral("+ &New Connection"), this);
    new_btn_->setObjectName(QStringLiteral("SidebarNewProfileButton"));
    new_btn_->setAccessibleName(QStringLiteral("New connection"));
    new_btn_->setAutoDefault(false);
    new_btn_->setMinimumHeight(26);
    root->addWidget(new_btn_);

    filter_edit_ = new QLineEdit(this);
    filter_edit_->setObjectName(QStringLiteral("SidebarFilterEdit"));
    filter_edit_->setAccessibleName(QStringLiteral("Filter saved profiles"));
    filter_edit_->setPlaceholderText(QStringLiteral("Filter profiles..."));
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->setMinimumHeight(24);
    root->addWidget(filter_edit_);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("ProfileSidebarTree"));
    tree_->setAccessibleName(QStringLiteral("Saved connection profiles"));
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(16, 16));
    tree_->setIndentation(14);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(tree_, 1);

    auto* bottom_row = new QHBoxLayout();
    bottom_row->setContentsMargins(0, 0, 0, 0);
    bottom_row->setSpacing(6);

    duplicate_btn_ = new QPushButton(QStringLiteral("D&uplicate"), this);
    duplicate_btn_->setAccessibleName(QStringLiteral("Duplicate profile"));
    duplicate_btn_->setAutoDefault(false);
    duplicate_btn_->setEnabled(false);

    delete_btn_ = new QPushButton(QStringLiteral("Delete P&rofile…"), this);
    delete_btn_->setAccessibleName(QStringLiteral("Delete profile"));
    delete_btn_->setAutoDefault(false);
    delete_btn_->setEnabled(false);

    bottom_row->addWidget(duplicate_btn_, 1);
    bottom_row->addWidget(delete_btn_, 1);
    root->addLayout(bottom_row);

    connect(new_btn_, &QPushButton::clicked, this, [this]() {
        clear_selection();
        emit new_connection_requested();
    });
    connect(filter_edit_, &QLineEdit::textChanged, this, &ProfileSidebarWidget::apply_filter);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &ProfileSidebarWidget::on_tree_selection_changed);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &ProfileSidebarWidget::on_item_double_clicked);
    connect(duplicate_btn_, &QPushButton::clicked, this, &ProfileSidebarWidget::duplicate_profile_requested);
    connect(delete_btn_, &QPushButton::clicked, this, &ProfileSidebarWidget::delete_profile_requested);
}

void ProfileSidebarWidget::set_profiles(
    std::span<const core::ConnectionProfile> profiles,
    const std::string& selected_id) {
    suppress_signals_ = true;
    tree_->clear();

    std::unordered_map<std::string, QTreeWidgetItem*> group_items;
    auto ensure_group = [&](const std::string& group_name) -> QTreeWidgetItem* {
        if (group_name.empty()) {
            return nullptr;
        }
        if (const auto it = group_items.find(group_name); it != group_items.end()) {
            return it->second;
        }
        const QString qgroup = QString::fromStdString(group_name);
        auto* g_item = new QTreeWidgetItem(tree_, {qgroup});
        g_item->setIcon(0, get_icon(IconId::Folder));
        g_item->setData(0, kRoleItemKind, kSidebarGroup);
        g_item->setData(0, kRoleGroupName, qgroup);
        g_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        g_item->setExpanded(true);
        group_items.emplace(group_name, g_item);
        return g_item;
    };

    QTreeWidgetItem* item_to_select = nullptr;
    for (const core::ConnectionProfile& p : profiles) {
        QTreeWidgetItem* parent_group = (p.group.has_value() && !p.group->empty())
            ? ensure_group(*p.group)
            : nullptr;

        const QString qname    = QString::fromStdString(p.name);
        const QString qid      = QString::fromStdString(p.id);
        const QString tooltip  = format_profile_tooltip(p);
        const auto eng         = drivers::engine_kind_from_string(p.engine);

        auto* p_item = (parent_group != nullptr)
            ? new QTreeWidgetItem(parent_group, {qname})
            : new QTreeWidgetItem(tree_, {qname});

        p_item->setIcon(0, get_icon(icon_for_engine(eng)));
        p_item->setData(0, kRoleItemKind, kSidebarProfile);
        p_item->setData(0, kRoleProfileId, qid);
        if (p.group.has_value()) {
            p_item->setData(0, kRoleGroupName, QString::fromStdString(*p.group));
        }
        p_item->setData(0, kRoleSummary, tooltip);
        p_item->setToolTip(0, tooltip);

        if (!selected_id.empty() && p.id == selected_id) {
            item_to_select = p_item;
        }
    }

    if (item_to_select != nullptr) {
        tree_->setCurrentItem(item_to_select);
    }

    if (!filter_edit_->text().isEmpty()) {
        apply_filter(filter_edit_->text());
    }

    const bool has_sel = (item_to_select != nullptr);
    duplicate_btn_->setEnabled(has_sel);
    delete_btn_->setEnabled(has_sel);
    suppress_signals_ = false;
}

void ProfileSidebarWidget::select_profile_by_id(const std::string& profile_id) {
    if (profile_id.empty()) {
        clear_selection();
        return;
    }

    const QString want = QString::fromStdString(profile_id);
    auto find_in = [&](auto& self, QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (item->data(0, kRoleItemKind).toInt() == kSidebarProfile &&
            item->data(0, kRoleProfileId).toString().compare(want, Qt::CaseInsensitive) == 0) {
            return item;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem* found = self(self, item->child(i))) {
                return found;
            }
        }
        return nullptr;
    };

    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* found = find_in(find_in, tree_->topLevelItem(i))) {
            tree_->setCurrentItem(found);
            return;
        }
    }
}

void ProfileSidebarWidget::clear_selection() {
    suppress_signals_ = true;
    tree_->clearSelection();
    tree_->setCurrentItem(nullptr);
    duplicate_btn_->setEnabled(false);
    delete_btn_->setEnabled(false);
    suppress_signals_ = false;
}

QString ProfileSidebarWidget::selected_profile_id() const {
    const QTreeWidgetItem* cur = tree_->currentItem();
    if (cur == nullptr || cur->data(0, kRoleItemKind).toInt() != kSidebarProfile) {
        return {};
    }
    return cur->data(0, kRoleProfileId).toString();
}

QString ProfileSidebarWidget::selected_group_name() const {
    const QTreeWidgetItem* cur = tree_->currentItem();
    if (cur == nullptr) {
        return {};
    }
    return cur->data(0, kRoleGroupName).toString();
}

int ProfileSidebarWidget::visible_profile_count() const {
    int count = 0;
    auto count_in = [&](auto& self, const QTreeWidgetItem* item) -> void {
        if (item->isHidden()) {
            return;
        }
        if (item->data(0, kRoleItemKind).toInt() == kSidebarProfile) {
            ++count;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            self(self, item->child(i));
        }
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        count_in(count_in, tree_->topLevelItem(i));
    }
    return count;
}

void ProfileSidebarWidget::set_controls_enabled(bool enabled, bool has_selected_profile) {
    new_btn_->setEnabled(enabled);
    filter_edit_->setEnabled(enabled);
    tree_->setEnabled(enabled);
    duplicate_btn_->setEnabled(enabled && has_selected_profile);
    delete_btn_->setEnabled(enabled && has_selected_profile);
}

void ProfileSidebarWidget::apply_filter(const QString& filter_text) {
    const QString needle = filter_text.trimmed();
    const bool empty_filter = needle.isEmpty();

    auto matches_profile = [&](const QTreeWidgetItem* item) -> bool {
        if (empty_filter) {
            return true;
        }
        if (item->text(0).contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
        if (item->data(0, kRoleGroupName).toString().contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
        if (item->data(0, kRoleSummary).toString().contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
        return false;
    };

    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        const int kind = top->data(0, kRoleItemKind).toInt();
        if (kind == kSidebarProfile) {
            top->setHidden(!matches_profile(top));
        } else if (kind == kSidebarGroup) {
            bool any_child_visible = false;
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem* child = top->child(c);
                const bool vis = matches_profile(child);
                child->setHidden(!vis);
                if (vis) {
                    any_child_visible = true;
                }
            }
            top->setHidden(!any_child_visible);
            if (any_child_visible) {
                top->setExpanded(true);
            }
        }
    }
}

void ProfileSidebarWidget::on_tree_selection_changed() {
    if (suppress_signals_) {
        return;
    }
    const QTreeWidgetItem* cur = tree_->currentItem();
    if (cur == nullptr || cur->data(0, kRoleItemKind).toInt() != kSidebarProfile) {
        duplicate_btn_->setEnabled(false);
        delete_btn_->setEnabled(false);
        return;
    }
    const QString id = cur->data(0, kRoleProfileId).toString();
    duplicate_btn_->setEnabled(true);
    delete_btn_->setEnabled(true);
    emit profile_selected(id);
}

void ProfileSidebarWidget::on_item_double_clicked(QTreeWidgetItem* item, int) {
    if (item == nullptr || item->data(0, kRoleItemKind).toInt() != kSidebarProfile) {
        return;
    }
    const QString id = item->data(0, kRoleProfileId).toString();
    if (!id.isEmpty()) {
        emit profile_activated(id);
    }
}

} // namespace capybaradb::ui
