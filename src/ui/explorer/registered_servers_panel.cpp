#include "ui/explorer/registered_servers_panel.hpp"

#include "ui/dialogs/connection_dialog.hpp"
#include "ui/shell/icon_catalog.hpp"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <unordered_map>

namespace capybaradb::ui {
namespace {

constexpr int kRoleKind      = Qt::UserRole;
constexpr int kRoleGroupPath = Qt::UserRole + 1;
constexpr int kRoleProfileId = Qt::UserRole + 2;

enum NodeKind : int {
    kKindRoot    = 0,
    kKindGroup   = 1,
    kKindProfile = 2,
};

drivers::ConnectionConfig to_driver_config(const core::ConnectionProfile& p, const std::string& password) {
    drivers::ConnectionConfig cfg;
    cfg.engine         = drivers::engine_kind_from_string(p.engine);
    cfg.host           = p.host;
    cfg.port           = static_cast<uint16_t>(p.port);
    cfg.database       = p.database;
    cfg.username       = p.username;
    cfg.password       = password;
    cfg.auth           = drivers::auth_kind_from_string(p.auth_mode);
    cfg.tls_mode       = drivers::tls_mode_from_string(p.ssl_mode);
    cfg.root_cert_path = p.root_certificate.value_or(std::string{});
    return cfg;
}

} // namespace

RegisteredServersPanel::RegisteredServersPanel(
    std::filesystem::path state_dir,
    core::CatalogWorkerPool* worker_pool,
    QWidget* parent)
    : QWidget(parent)
    , state_dir_(std::move(state_dir))
    , worker_pool_(worker_pool) {
    setObjectName(QStringLiteral("RegisteredServersPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "QWidget#RegisteredServersPanel { background-color: #F5FAFE; color: #1E1E1E; }"
        "QWidget#RegServersToolbar { background-color: #D5E5EF; border-bottom: 1px solid #B5C5D2; }"
        "QTreeWidget#RegServersTree { background-color: #F5FAFE; border: none; padding: 4px; }"));
    build_ui();
    refresh();
}

void RegisteredServersPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("RegServersToolbar"));
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(4, 2, 4, 2);
    tb->setSpacing(4);

    auto make_btn = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton(toolbar);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        tb->addWidget(b);
        return b;
    };

    auto* new_grp_btn = make_btn(QStringLiteral("New Group…"), QStringLiteral("New Server Group in the selected group"));
    auto* reg_btn     = make_btn(QStringLiteral("Register…"), QStringLiteral("New Server Registration in the selected group"));
    auto* refresh_btn = make_btn(QStringLiteral("Refresh"), QStringLiteral("Reload registered servers"));
    tb->addStretch(1);
    root->addWidget(toolbar);

    connect(new_grp_btn, &QToolButton::clicked, this, &RegisteredServersPanel::on_new_group);
    connect(reg_btn, &QToolButton::clicked, this, &RegisteredServersPanel::on_new_registration);
    connect(refresh_btn, &QToolButton::clicked, this, &RegisteredServersPanel::refresh);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("RegServersTree"));
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(16, 16));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &RegisteredServersPanel::on_context_menu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        on_item_activated(item);
    });
    root->addWidget(tree_, 1);

    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setContentsMargins(6, 3, 6, 4);
    feedback_->setStyleSheet(QStringLiteral("color: #4B5964; font-size: 11px;"));
    root->addWidget(feedback_);
}

void RegisteredServersPanel::refresh() {
    tree_->clear();
    profiles_.clear();

    const std::filesystem::path profiles_path = state_dir_ / "connections.json";
    std::string err;
    profiles_ = core::load_connection_profiles(profiles_path, &err);
    std::vector<std::string> groups;
    if (err.empty()) {
        groups = core::load_connection_profile_groups(profiles_path, &err);
    }

    std::unordered_map<std::string, QTreeWidgetItem*> group_nodes;
    auto ensure_group = [&](const std::string& full_path) -> QTreeWidgetItem* {
        if (full_path.empty()) return nullptr;
        if (const auto it = group_nodes.find(full_path); it != group_nodes.end()) {
            return it->second;
        }
        QTreeWidgetItem* parent = nullptr;
        std::string prefix;
        size_t pos = 0;
        while (pos <= full_path.size()) {
            const size_t slash = full_path.find('/', pos);
            const std::string seg = (slash == std::string::npos)
                ? full_path.substr(pos)
                : full_path.substr(pos, slash - pos);
            if (!prefix.empty()) prefix.push_back('/');
            prefix += seg;
            auto it = group_nodes.find(prefix);
            if (it == group_nodes.end()) {
                auto* node = (parent != nullptr)
                    ? new QTreeWidgetItem(parent, {QString::fromStdString(seg)})
                    : new QTreeWidgetItem(tree_, {QString::fromStdString(seg)});
                node->setIcon(0, get_icon(IconId::Folder));
                node->setData(0, kRoleKind, kKindGroup);
                node->setData(0, kRoleGroupPath, QString::fromStdString(prefix));
                group_nodes.emplace(prefix, node);
                parent = node;
            } else {
                parent = it->second;
            }
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        return parent;
    };

    for (const std::string& g : groups) {
        (void)ensure_group(g);
    }

    for (const core::ConnectionProfile& p : profiles_) {
        QTreeWidgetItem* parent = p.group.has_value() && !p.group->empty()
            ? ensure_group(*p.group)
            : nullptr;
        auto* item = (parent != nullptr)
            ? new QTreeWidgetItem(parent, {QString::fromStdString(p.name)})
            : new QTreeWidgetItem(tree_, {QString::fromStdString(p.name)});
        const auto eng = drivers::engine_kind_from_string(p.engine);
        const bool is_mssql  = (eng == drivers::EngineKind::SqlServer);
        const bool is_sqlite = (eng == drivers::EngineKind::Sqlite);
        const IconId icon_id = is_sqlite ? IconId::EngineSqlite
                             : (is_mssql ? IconId::EngineMssql : IconId::EnginePostgres);
        item->setIcon(0, get_icon(icon_id));
        item->setData(0, kRoleKind, kKindProfile);
        item->setData(0, kRoleProfileId, QString::fromStdString(p.id));
        if (is_sqlite) {
            item->setToolTip(0, QStringLiteral("SQLite · %1").arg(QString::fromStdString(p.database)));
        } else {
            item->setToolTip(0, QStringLiteral("%1 · %2:%3 / %4 (%5)")
                .arg(is_mssql ? QStringLiteral("SQL Server") : QStringLiteral("PostgreSQL"),
                     QString::fromStdString(p.host))
                .arg(p.port)
                .arg(QString::fromStdString(p.database), QString::fromStdString(p.username)));
        }
    }

    for (const auto& [_, node] : group_nodes) {
        node->setExpanded(true);
    }

    if (!err.empty()) {
        feedback_->setText(QString::fromStdString(err));
    } else {
        feedback_->setText(QStringLiteral("%1 registered server(s).").arg(profiles_.size()));
    }
}

bool RegisteredServersPanel::create_group(const QString& group_path) {
    std::string err;
    if (!core::add_connection_profile_group(state_dir_ / "connections.json", group_path.trimmed().toStdString(), &err)) {
        feedback_->setText(QString::fromStdString(err));
        return false;
    }
    refresh();
    feedback_->setText(QStringLiteral("Created server group \"%1\".").arg(group_path.trimmed()));
    return true;
}

QString RegisteredServersPanel::selected_group_path() const {
    const QTreeWidgetItem* cur = tree_->currentItem();
    if (cur == nullptr) return {};
    const int kind = cur->data(0, kRoleKind).toInt();
    if (kind == kKindGroup) {
        return cur->data(0, kRoleGroupPath).toString();
    }
    if (kind == kKindProfile && cur->parent() != nullptr) {
        return cur->parent()->data(0, kRoleGroupPath).toString();
    }
    return {};
}

std::optional<core::ConnectionProfile> RegisteredServersPanel::profile_for_item(const QTreeWidgetItem* item) const {
    if (item == nullptr || item->data(0, kRoleKind).toInt() != kKindProfile) {
        return std::nullopt;
    }
    const std::string id = item->data(0, kRoleProfileId).toString().toStdString();
    for (const core::ConnectionProfile& p : profiles_) {
        if (p.id == id) return p;
    }
    return std::nullopt;
}

std::optional<drivers::ConnectionConfig> RegisteredServersPanel::config_from_profile(const core::ConnectionProfile& p) {
    const auto eng = drivers::engine_kind_from_string(p.engine);
    if (eng == drivers::EngineKind::Sqlite) {
        return to_driver_config(p, {});
    }
    std::string pwd;
    if (drivers::auth_kind_from_string(p.auth_mode) == drivers::AuthKind::Password) {
        if (p.protected_password.has_value() && !p.protected_password->empty()) {
            if (auto plain = p.reveal_password(); plain.has_value()) {
                pwd = std::move(*plain);
            }
        }
        if (pwd.empty()) {
            // Open ConnectionDialog prefilled so user can supply password securely
            ConnectionDialog dlg(worker_pool_, state_dir_, this);
            dlg.prefill(to_driver_config(p, {}));
            if (dlg.exec() != QDialog::Accepted) {
                return std::nullopt;
            }
            return dlg.connection_config();
        }
    }
    return to_driver_config(p, pwd);
}

void RegisteredServersPanel::on_new_group() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        QStringLiteral("New Server Group"),
        QStringLiteral("Group name:"),
        QLineEdit::Normal,
        QString{},
        &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    const QString parent_grp = selected_group_path();
    const QString full_path = parent_grp.isEmpty() ? name : (parent_grp + QLatin1Char('/') + name);
    (void)create_group(full_path);
}

void RegisteredServersPanel::on_new_registration() {
    ConnectionDialog dlg(worker_pool_, state_dir_, this);
    if (dlg.exec() == QDialog::Accepted) {
        refresh();
        emit connect_in_explorer_requested(dlg.connection_config());
    } else {
        refresh(); // User may have clicked Save Profile then Cancel
    }
}

void RegisteredServersPanel::on_item_activated(QTreeWidgetItem* item) {
    const auto p = profile_for_item(item);
    if (!p.has_value()) return;
    if (const auto cfg = config_from_profile(*p)) {
        emit connect_in_explorer_requested(*cfg);
    }
}

void RegisteredServersPanel::on_context_menu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (item != nullptr) {
        tree_->setCurrentItem(item);
    }

    QMenu menu(this);
    const int kind = item ? item->data(0, kRoleKind).toInt() : kKindRoot;
    if (kind == kKindProfile) {
        const auto prof = profile_for_item(item);
        if (!prof.has_value()) return;
        QAction* act_oe = menu.addAction(QStringLiteral("Object Explorer"));
        connect(act_oe, &QAction::triggered, this, [this, p = *prof]() {
            if (const auto cfg = config_from_profile(p)) emit connect_in_explorer_requested(*cfg);
        });
        QAction* act_nq = menu.addAction(QStringLiteral("New Query"));
        connect(act_nq, &QAction::triggered, this, [this, p = *prof]() {
            if (const auto cfg = config_from_profile(p)) emit new_query_requested(*cfg);
        });
        menu.addSeparator();
        QAction* act_del = menu.addAction(QStringLiteral("Delete Registration…"));
        connect(act_del, &QAction::triggered, this, [this, p = *prof]() {
            const auto ans = QMessageBox::question(
                this,
                QStringLiteral("Delete Server Registration"),
                QStringLiteral("Remove saved profile \"%1\"?").arg(QString::fromStdString(p.name)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (ans != QMessageBox::Yes) return;
            std::string err;
            if (!core::remove_connection_profile(state_dir_ / "connections.json", p.id, &err)) {
                feedback_->setText(QString::fromStdString(err));
            } else {
                refresh();
            }
        });
    } else {
        QAction* act_ng = menu.addAction(QStringLiteral("New Server Group…"));
        connect(act_ng, &QAction::triggered, this, &RegisteredServersPanel::on_new_group);
        QAction* act_reg = menu.addAction(QStringLiteral("New Server Registration…"));
        connect(act_reg, &QAction::triggered, this, &RegisteredServersPanel::on_new_registration);
        if (kind == kKindGroup) {
            const QString gpath = item->data(0, kRoleGroupPath).toString();
            menu.addSeparator();
            QAction* act_dg = menu.addAction(QStringLiteral("Delete Group…"));
            connect(act_dg, &QAction::triggered, this, [this, gpath]() {
                const auto ans = QMessageBox::question(
                    this,
                    QStringLiteral("Delete Server Group"),
                    QStringLiteral("Delete group \"%1\"? Profiles inside will move to Local Server Groups.").arg(gpath),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (ans != QMessageBox::Yes) return;
                std::string err;
                if (!core::remove_connection_profile_group(state_dir_ / "connections.json", gpath.toStdString(), &err)) {
                    feedback_->setText(QString::fromStdString(err));
                } else {
                    refresh();
                }
            });
        }
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace capybaradb::ui
