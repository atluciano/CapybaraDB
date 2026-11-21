#include "ui/dialogs/connection_dialog.hpp"

#include "core/config/connection_uri_parser.hpp"
#include "drivers/common/driver_registry.hpp"
#include "ui/shell/icon_catalog.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>

namespace capybaradb::ui {
namespace {

IconId badge_icon_for_engine(drivers::EngineKind engine) noexcept {
    switch (engine) {
    case drivers::EngineKind::PostgreSql: return IconId::EnginePostgres;
    case drivers::EngineKind::SqlServer:  return IconId::EngineMssql;
    case drivers::EngineKind::Sqlite:     return IconId::EngineSqlite;
    }
    return IconId::EnginePostgres;
}

constexpr size_t engine_to_idx(drivers::EngineKind engine) noexcept {
    switch (engine) {
    case drivers::EngineKind::PostgreSql: return 0;
    case drivers::EngineKind::SqlServer:  return 1;
    case drivers::EngineKind::Sqlite:     return 2;
    }
    return 0;
}

} // namespace

ConnectionDialog::ConnectionDialog(
    core::CatalogWorkerPool* worker_pool,
    std::filesystem::path state_dir,
    QWidget* parent)
    : QDialog(parent)
    , state_dir_(state_dir.empty() ? core::state_directory() : std::move(state_dir))
    , worker_pool_(worker_pool) {
    setObjectName(QStringLiteral("ConnectionDialog"));
    setWindowTitle(QStringLiteral("Connect to Server"));
    setMinimumSize(620, 440);
    resize(720, 520);

    build_ui();

    const core::ConnectionDialogPreferences prefs =
        core::load_connection_dialog_preferences(state_dir_ / "connect-dialog.json");
    if (prefs.width.has_value() && prefs.height.has_value()) {
        resize(
            std::max(minimumWidth(), static_cast<int>(*prefs.width)),
            std::max(minimumHeight(), static_cast<int>(*prefs.height)));
    }

    load_profiles(prefs.last_profile.value_or(std::string{}));

    QTimer::singleShot(0, this, [this]() {
        const auto& desc = drivers::driver_ui_descriptor(current_engine_);
        if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
            if (file_page_->file_path_edit()->isVisible()) {
                file_page_->file_path_edit()->setFocus();
            } else {
                connect_btn_->setFocus();
            }
        } else if (!selected_profile_id_.empty() &&
                   server_page_->password_edit()->text().isEmpty() &&
                   server_page_->password_edit()->isVisible()) {
            server_page_->password_edit()->setFocus();
        } else {
            server_page_->host_edit()->setFocus();
        }
    });
}

ConnectionDialog::~ConnectionDialog() {
    if (test_cancel_) {
        test_cancel_->cancel();
    }
    if (server_page_ != nullptr) {
        server_page_->clear_secrets();
    }
    owned_pool_.reset();
    (void)drivers::unload_unused_driver_modules(std::chrono::milliseconds{0});
}

bool ConnectionDialog::disable_query_history() const {
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        return file_page_->disable_history_check()->isChecked();
    }
    return server_page_->disable_history_check()->isChecked();
}

QLineEdit* ConnectionDialog::database_edit() const noexcept {
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        return file_page_->file_path_edit();
    }
    return server_page_->database_edit();
}

QCheckBox* ConnectionDialog::disable_history_check() const noexcept {
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        return file_page_->disable_history_check();
    }
    return server_page_->disable_history_check();
}

QToolButton* ConnectionDialog::engine_button(drivers::EngineKind kind) const noexcept {
    return engine_buttons_[engine_to_idx(kind)];
}

void ConnectionDialog::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Slim Header Banner (#E4EFF6, 44px height, 1px bottom border #B5C5D2)
    auto* banner = new QFrame(this);
    banner->setObjectName(QStringLiteral("DialogBanner"));
    banner->setFixedHeight(46);
    auto* banner_layout = new QHBoxLayout(banner);
    banner_layout->setContentsMargins(16, 6, 16, 6);
    banner_layout->setSpacing(12);

    auto* title_lbl = new QLabel(QStringLiteral("CapybaraDB"), banner);
    title_lbl->setObjectName(QStringLiteral("DialogBannerTitle"));
    banner_layout->addWidget(title_lbl);

    auto* sep_lbl = new QLabel(QStringLiteral("·"), banner);
    sep_lbl->setObjectName(QStringLiteral("DialogMutedText"));
    banner_layout->addWidget(sep_lbl);

    subtitle_label_ = new QLabel(QStringLiteral("Connect to a PostgreSQL server"), banner);
    subtitle_label_->setObjectName(QStringLiteral("DialogBannerSubtitle"));
    banner_layout->addWidget(subtitle_label_, 1);

    root->addWidget(banner);

    // 2. Master-Detail Body (Left Sidebar + Right Detail Pane)
    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("ConnectionDialogBody"));
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    sidebar_ = new ProfileSidebarWidget(body);
    body_layout->addWidget(sidebar_);

    auto* v_divider = new QFrame(body);
    v_divider->setObjectName(QStringLiteral("ConnectionSidebarDivider"));
    v_divider->setFrameShape(QFrame::VLine);
    v_divider->setFixedWidth(1);
    body_layout->addWidget(v_divider);

    auto* detail_pane = new QWidget(body);
    detail_pane->setObjectName(QStringLiteral("ConnectionDetailPane"));
    auto* detail_layout = new QVBoxLayout(detail_pane);
    detail_layout->setContentsMargins(16, 10, 16, 8);
    detail_layout->setSpacing(8);

    // 2a. Engine Selector Bar (checkable buttons built from all_driver_ui_descriptors())
    auto* engine_bar = new QWidget(detail_pane);
    auto* engine_row = new QHBoxLayout(engine_bar);
    engine_row->setContentsMargins(0, 0, 0, 0);
    engine_row->setSpacing(6);

    auto* engine_lbl = new QLabel(QStringLiteral("Engine:"), engine_bar);
    engine_row->addWidget(engine_lbl);

    engine_btn_group_ = new QButtonGroup(this);
    engine_btn_group_->setExclusive(true);

    for (const auto& desc : drivers::all_driver_ui_descriptors()) {
        auto* btn = new QToolButton(engine_bar);
        btn->setObjectName(QStringLiteral("EngineSelectorButton"));
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setIcon(get_icon(badge_icon_for_engine(desc.engine)));
        btn->setIconSize(QSize(16, 16));
        btn->setText(QString::fromUtf8(desc.display_name.data(), static_cast<qsizetype>(desc.display_name.size())));
        btn->setMinimumHeight(28);
        btn->setAccessibleName(btn->text());
        const int id = static_cast<int>(desc.engine);
        engine_btn_group_->addButton(btn, id);
        engine_buttons_[engine_to_idx(desc.engine)] = btn;
        engine_row->addWidget(btn);
    }
    engine_buttons_[0]->setChecked(true);
    engine_row->addStretch(1);
    detail_layout->addWidget(engine_bar);

    // 2b. Profile Name & Folder/Group Row
    auto* prof_meta_bar = new QWidget(detail_pane);
    auto* prof_meta_row = new QHBoxLayout(prof_meta_bar);
    prof_meta_row->setContentsMargins(0, 0, 0, 0);
    prof_meta_row->setSpacing(8);

    auto* pname_lbl = new QLabel(QStringLiteral("Profile &name:"), prof_meta_bar);
    profile_name_edit_ = new QLineEdit(prof_meta_bar);
    profile_name_edit_->setAccessibleName(QStringLiteral("Profile name"));
    profile_name_edit_->setPlaceholderText(QStringLiteral("e.g. Local Dev or Production Analytics"));
    profile_name_edit_->setMinimumHeight(25);
    pname_lbl->setBuddy(profile_name_edit_);

    auto* pgroup_lbl = new QLabel(QStringLiteral("&Folder:"), prof_meta_bar);
    profile_group_combo_ = new QComboBox(prof_meta_bar);
    profile_group_combo_->setAccessibleName(QStringLiteral("Profile group"));
    profile_group_combo_->setEditable(true);
    profile_group_combo_->setMinimumHeight(25);
    profile_group_combo_->setMinimumWidth(115);
    if (profile_group_combo_->lineEdit() != nullptr) {
        profile_group_combo_->lineEdit()->setPlaceholderText(QStringLiteral("(None)"));
    }
    pgroup_lbl->setBuddy(profile_group_combo_);

    prof_meta_row->addWidget(pname_lbl);
    prof_meta_row->addWidget(profile_name_edit_, 2);
    prof_meta_row->addWidget(pgroup_lbl);
    prof_meta_row->addWidget(profile_group_combo_, 1);
    detail_layout->addWidget(prof_meta_bar);

    // 2c. QStackedWidget with only 2 reusable pages (ServerConnectionPage + FileConnectionPage)
    pages_stack_ = new QStackedWidget(detail_pane);
    pages_stack_->setObjectName(QStringLiteral("ConnectionPagesStack"));
    server_page_ = new ServerConnectionPage(pages_stack_);
    file_page_   = new FileConnectionPage(pages_stack_);
    pages_stack_->addWidget(server_page_); // index 0: NetworkServer
    pages_stack_->addWidget(file_page_);   // index 1: LocalFile
    detail_layout->addWidget(pages_stack_, 1);

    // 2d. Integrated Status Row (icon + word-wrapped status label)
    auto* status_box = new QFrame(detail_pane);
    status_box->setObjectName(QStringLiteral("ConnectionStatusBar"));
    auto* status_row = new QHBoxLayout(status_box);
    status_row->setContentsMargins(6, 4, 6, 4);
    status_row->setSpacing(6);

    status_icon_label_ = new QLabel(status_box);
    status_icon_label_->setFixedSize(16, 16);
    status_icon_label_->setVisible(false);
    status_row->addWidget(status_icon_label_, 0, Qt::AlignTop);

    status_label_ = new QLabel(status_box);
    status_label_->setObjectName(QStringLiteral("DialogStatusLabel"));
    status_label_->setWordWrap(true);
    status_row->addWidget(status_label_, 1);

    detail_layout->addWidget(status_box);
    body_layout->addWidget(detail_pane, 1);
    root->addWidget(body, 1);

    // 3. Synchronized hidden combo proxies for existing test & programmatic contracts
    profiles_combo_ = new QComboBox(this);
    profiles_combo_->setObjectName(QStringLiteral("ProfilesComboProxy"));
    profiles_combo_->setVisible(false);

    engine_combo_ = new QComboBox(this);
    engine_combo_->setObjectName(QStringLiteral("EngineComboProxy"));
    engine_combo_->setVisible(false);
    for (const auto& desc : drivers::all_driver_ui_descriptors()) {
        engine_combo_->addItem(
            QString::fromUtf8(desc.display_name.data(), static_cast<qsizetype>(desc.display_name.size())),
            static_cast<int>(desc.engine));
    }

    // 4. Footer action buttons
    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("ConnectionDialogFooter"));
    auto* foot_layout = new QHBoxLayout(footer);
    foot_layout->setContentsMargins(14, 8, 16, 10);
    foot_layout->setSpacing(8);

    cancel_test_btn_ = new QPushButton(QStringLiteral("Cancel Test (&X)"), footer);
    cancel_test_btn_->setVisible(false);
    cancel_test_btn_->setAutoDefault(false);
    foot_layout->addWidget(cancel_test_btn_);

    foot_layout->addStretch(1);

    test_btn_ = new QPushButton(QStringLiteral("&Test"), footer);
    test_btn_->setMinimumWidth(76);
    test_btn_->setAutoDefault(false);
    foot_layout->addWidget(test_btn_);

    save_profile_btn_ = new QPushButton(QStringLiteral("Sa&ve Profile"), footer);
    save_profile_btn_->setMinimumWidth(94);
    save_profile_btn_->setAutoDefault(false);
    foot_layout->addWidget(save_profile_btn_);

    connect_btn_ = new QPushButton(QStringLiteral("C&onnect"), footer);
    connect_btn_->setMinimumWidth(88);
    connect_btn_->setDefault(true);
    foot_layout->addWidget(connect_btn_);

    cancel_btn_ = new QPushButton(QStringLiteral("&Cancel"), footer);
    cancel_btn_->setMinimumWidth(82);
    cancel_btn_->setAutoDefault(false);
    foot_layout->addWidget(cancel_btn_);

    root->addWidget(footer);

    // Wire signals
    connect(sidebar_, &ProfileSidebarWidget::profile_selected, this, &ConnectionDialog::on_profile_selected_by_id);
    connect(sidebar_, &ProfileSidebarWidget::profile_activated, this, &ConnectionDialog::on_profile_double_clicked);
    connect(sidebar_, &ProfileSidebarWidget::new_connection_requested, this, &ConnectionDialog::on_new_profile);
    connect(sidebar_, &ProfileSidebarWidget::duplicate_profile_requested, this, &ConnectionDialog::on_duplicate_profile);
    connect(sidebar_, &ProfileSidebarWidget::delete_profile_requested, this, &ConnectionDialog::on_delete_profile);

    connect(engine_btn_group_, &QButtonGroup::idClicked, this, [this](int id) {
        on_engine_selected(static_cast<drivers::EngineKind>(id));
    });

    connect(profiles_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncing_form_) {
            return;
        }
        if (index <= 0) {
            on_new_profile();
        } else {
            const QString id = profiles_combo_->itemData(index).toString();
            select_profile_by_id(id);
        }
    });

    connect(engine_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncing_form_ || index < 0) {
            return;
        }
        const auto kind = static_cast<drivers::EngineKind>(engine_combo_->itemData(index).toInt());
        select_engine(kind);
    });

    connect(server_page_, &ServerConnectionPage::paste_url_requested, this, &ConnectionDialog::on_paste_url);
    connect(server_page_, &ServerConnectionPage::disable_history_toggled, this, [this](bool checked) {
        if (!syncing_form_ && file_page_->disable_history_check()->isChecked() != checked) {
            const bool old = syncing_form_;
            syncing_form_ = true;
            file_page_->disable_history_check()->setChecked(checked);
            syncing_form_ = old;
        }
    });
    connect(file_page_, &FileConnectionPage::disable_history_toggled, this, [this](bool checked) {
        if (!syncing_form_ && server_page_->disable_history_check()->isChecked() != checked) {
            const bool old = syncing_form_;
            syncing_form_ = true;
            server_page_->disable_history_check()->setChecked(checked);
            syncing_form_ = old;
        }
    });

    connect(save_profile_btn_, &QPushButton::clicked, this, &ConnectionDialog::on_save_profile);
    connect(test_btn_, &QPushButton::clicked, this, &ConnectionDialog::start_test_connection);
    connect(cancel_test_btn_, &QPushButton::clicked, this, &ConnectionDialog::cancel_test_connection);
    connect(connect_btn_, &QPushButton::clicked, this, &ConnectionDialog::on_accept_connect);
    connect(cancel_btn_, &QPushButton::clicked, this, &ConnectionDialog::reject);

    update_engine_specific_ui(false);
}

void ConnectionDialog::select_engine(drivers::EngineKind engine) {
    if (QToolButton* btn = engine_button(engine)) {
        btn->setChecked(true);
    }
    on_engine_selected(engine);
}

void ConnectionDialog::on_engine_selected(drivers::EngineKind engine) {
    if (syncing_form_) {
        current_engine_ = engine;
        return;
    }
    current_engine_ = engine;
    const bool old = syncing_form_;
    syncing_form_ = true;
    engine_combo_->setCurrentIndex(static_cast<int>(engine_to_idx(engine)));
    if (QToolButton* btn = engine_button(engine)) {
        btn->setChecked(true);
    }
    syncing_form_ = old;

    update_engine_specific_ui(true);
}

void ConnectionDialog::update_engine_specific_ui(bool apply_smart_defaults) {
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    subtitle_label_->setText(QString::fromUtf8(
        desc.subtitle.data(),
        static_cast<qsizetype>(desc.subtitle.size())));

    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        file_page_->configure_for_engine(desc);
        pages_stack_->setCurrentWidget(file_page_);
    } else {
        server_page_->configure_for_engine(desc, apply_smart_defaults);
        pages_stack_->setCurrentWidget(server_page_);
    }
}

void ConnectionDialog::load_profiles(const std::string& select_id) {
    syncing_form_ = true;
    profiles_combo_->clear();
    profiles_combo_->addItem(QStringLiteral("(New connection)"), QString{});

    const std::filesystem::path profiles_path = state_dir_ / "connections.json";
    std::string load_err;
    profiles_ = core::load_connection_profiles(profiles_path, &load_err);

    QString cur_group = profile_group_combo_->currentText();
    profile_group_combo_->clear();
    profile_group_combo_->addItem(QString{});
    for (const std::string& g : core::load_connection_profile_groups(profiles_path)) {
        profile_group_combo_->addItem(QString::fromStdString(g));
    }
    profile_group_combo_->setEditText(cur_group);

    int target_combo_idx = 0;
    const core::ConnectionProfile* matched_profile = nullptr;
    for (size_t i = 0; i < profiles_.size(); ++i) {
        const core::ConnectionProfile& p = profiles_[i];
        profiles_combo_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
        if (!select_id.empty() && p.id == select_id) {
            target_combo_idx = static_cast<int>(i + 1);
            matched_profile = &p;
        }
    }

    profiles_combo_->setCurrentIndex(target_combo_idx);
    sidebar_->set_profiles(profiles_, matched_profile ? matched_profile->id : std::string{});
    syncing_form_ = false;

    if (!load_err.empty()) {
        set_status_text(
            QStringLiteral("Profiles could not be loaded. The existing file was preserved. ") +
                QString::fromStdString(load_err),
            theme::kErrorTextFg,
            StatusIconState::Error);
    }

    if (matched_profile != nullptr) {
        apply_profile(*matched_profile);
    } else {
        selected_profile_id_.clear();
        sidebar_->set_controls_enabled(!testing_, false);
    }
}

void ConnectionDialog::on_profile_selected_by_id(const QString& profile_id) {
    if (syncing_form_) {
        return;
    }
    const std::string want = profile_id.toStdString();
    for (size_t i = 0; i < profiles_.size(); ++i) {
        if (profiles_[i].id == want) {
            const bool old = syncing_form_;
            syncing_form_ = true;
            profiles_combo_->setCurrentIndex(static_cast<int>(i + 1));
            syncing_form_ = old;
            apply_profile(profiles_[i]);
            return;
        }
    }
}

void ConnectionDialog::on_profile_double_clicked(const QString& profile_id) {
    if (testing_) {
        return;
    }
    on_profile_selected_by_id(profile_id);
    if (selected_profile_id_.empty()) {
        return;
    }
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    if (desc.storage_kind == drivers::ConnectionStorageKind::NetworkServer) {
        const auto auth = desc.supports_integrated_auth
            ? static_cast<drivers::AuthKind>(server_page_->auth_combo()->currentData().toInt())
            : drivers::AuthKind::Password;
        if (auth == drivers::AuthKind::Password && server_page_->password_edit()->text().isEmpty()) {
            server_page_->tab_widget()->setCurrentIndex(0);
            server_page_->password_edit()->setFocus();
            set_status_text(
                QStringLiteral("Enter the password for \"%1\" and click Connect.")
                    .arg(profile_name_edit_->text()),
                theme::kMutedTextFg);
            return;
        }
    }
    on_accept_connect();
}

void ConnectionDialog::apply_profile(const core::ConnectionProfile& p) {
    selected_profile_id_ = p.id;
    save_dialog_preferences(&selected_profile_id_);

    syncing_form_ = true;
    server_page_->clear_secrets();

    profile_name_edit_->setText(QString::fromStdString(p.name));
    profile_group_combo_->setEditText(p.group.has_value() ? QString::fromStdString(*p.group) : QString{});

    current_engine_ = drivers::engine_kind_from_string(p.engine);
    const size_t eng_idx = engine_to_idx(current_engine_);
    engine_combo_->setCurrentIndex(static_cast<int>(eng_idx));
    if (engine_buttons_[eng_idx] != nullptr) {
        engine_buttons_[eng_idx]->setChecked(true);
    }

    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        file_page_->configure_for_engine(desc);
        file_page_->apply_database_value(QString::fromStdString(p.database), p.disable_query_history);
        server_page_->disable_history_check()->setChecked(p.disable_query_history);
        server_page_->remember_password_check()->setChecked(false);
        set_status_text(QString{}, theme::kErrorTextFg);
    } else {
        server_page_->host_edit()->setText(QString::fromStdString(p.host));
        server_page_->port_edit()->setText(QString::number(p.port));
        server_page_->database_edit()->setText(QString::fromStdString(p.database));

        const auto auth = drivers::auth_kind_from_string(p.auth_mode);
        server_page_->auth_combo()->setCurrentIndex(
            auth == drivers::AuthKind::IntegratedWindowsOrKerberos ? 1 : 0);

        server_page_->username_edit()->setText(QString::fromStdString(p.username));
        server_page_->ssl_combo()->setCurrentText(QString::fromStdString(p.ssl_mode));
        server_page_->root_cert_edit()->setText(
            p.root_certificate.has_value() ? QString::fromStdString(*p.root_certificate) : QString{});
        server_page_->disable_history_check()->setChecked(p.disable_query_history);
        file_page_->disable_history_check()->setChecked(p.disable_query_history);
        server_page_->mark_fields_unedited();

        server_page_->remember_password_check()->setChecked(false);
        set_status_text(QString{}, theme::kErrorTextFg);

        if (p.protected_password.has_value() && !p.protected_password->empty()) {
            const auto revealed = p.reveal_password();
            if (revealed.has_value()) {
                server_page_->password_edit()->setText(QString::fromStdString(*revealed));
                server_page_->remember_password_check()->setChecked(true);
            } else {
                set_status_text(
                    QStringLiteral("Saved password is unavailable. Re-enter it and save the profile to replace it."),
                    theme::kErrorTextFg,
                    StatusIconState::Error);
            }
        }
    }
    syncing_form_ = false;

    sidebar_->set_controls_enabled(!testing_, true);
    update_engine_specific_ui(false);
}

void ConnectionDialog::on_new_profile() {
    syncing_form_ = true;
    profiles_combo_->setCurrentIndex(0);
    const QString keep_group = sidebar_->selected_group_name();
    sidebar_->clear_selection();
    selected_profile_id_.clear();
    profile_name_edit_->clear();
    profile_group_combo_->setEditText(keep_group);

    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    server_page_->reset_for_new_profile(desc);
    file_page_->reset_for_new_profile(desc);
    syncing_form_ = false;

    sidebar_->set_controls_enabled(!testing_, false);
    update_engine_specific_ui(true);
    set_status_text(
        QStringLiteral("New profile. Enter settings and choose Save Profile."),
        theme::kMutedTextFg);
}

void ConnectionDialog::on_duplicate_profile() {
    if (testing_ || selected_profile_id_.empty()) {
        return;
    }
    selected_profile_id_.clear();
    syncing_form_ = true;
    profiles_combo_->setCurrentIndex(0);
    sidebar_->clear_selection();
    const QString base_name = profile_name_edit_->text().trimmed();
    profile_name_edit_->setText(
        base_name.isEmpty() ? QStringLiteral("Copy") : (base_name + QStringLiteral(" (Copy)")));
    syncing_form_ = false;

    profile_name_edit_->setFocus();
    profile_name_edit_->selectAll();
    set_status_text(
        QStringLiteral("Duplicated profile settings. Click Save Profile to store the copy."),
        theme::kMutedTextFg);
}

void ConnectionDialog::on_paste_url() {
    if (testing_) {
        return;
    }
    QString clip;
    if (const QClipboard* cb = QGuiApplication::clipboard()) {
        clip = cb->text().trimmed();
    }
    if (!clip.isEmpty() && apply_connection_uri(clip)) {
        return;
    }

    bool ok = false;
    const QString entered = QInputDialog::getText(
        this,
        QStringLiteral("Paste Connection URL or String"),
        QStringLiteral("Connection URL (postgresql://, mssql://, sqlite:///) or key=value string:"),
        QLineEdit::Normal,
        clip,
        &ok).trimmed();
    if (!ok || entered.isEmpty()) {
        return;
    }
    if (!apply_connection_uri(entered)) {
        set_status_text(
            QStringLiteral("Could not parse connection URL or connection string."),
            theme::kErrorTextFg,
            StatusIconState::Error);
    }
}

bool ConnectionDialog::apply_connection_uri(const QString& uri_text) {
    const auto parsed = core::parse_connection_uri(uri_text.toStdString());
    if (!parsed.has_value()) {
        return false;
    }

    const drivers::ConnectionConfig& cfg = parsed->config;
    if (parsed->has_engine) {
        select_engine(cfg.engine);
    }

    const auto& desc = drivers::driver_ui_descriptor(current_engine_);
    syncing_form_ = true;
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        file_page_->apply_database_value(
            QString::fromStdString(cfg.database),
            file_page_->disable_history_check()->isChecked());
    } else {
        if (parsed->has_host) {
            server_page_->host_edit()->setText(QString::fromStdString(cfg.host));
        }
        if (parsed->has_port) {
            server_page_->port_edit()->setText(QString::number(cfg.port));
        }
        if (parsed->has_database) {
            server_page_->database_edit()->setText(QString::fromStdString(cfg.database));
        }
        if (parsed->has_auth && desc.supports_integrated_auth) {
            server_page_->auth_combo()->setCurrentIndex(
                cfg.auth == drivers::AuthKind::IntegratedWindowsOrKerberos ? 1 : 0);
        }
        if (parsed->has_username) {
            server_page_->username_edit()->setText(QString::fromStdString(cfg.username));
        }
        if (parsed->has_password) {
            server_page_->password_edit()->setText(QString::fromStdString(cfg.password));
        }
        if (parsed->has_tls_mode) {
            server_page_->ssl_combo()->setCurrentText(
                QString::fromStdString(std::string(drivers::tls_mode_to_string(cfg.tls_mode))));
        }
        if (!cfg.root_cert_path.empty() && desc.supports_root_cert) {
            server_page_->root_cert_edit()->setText(QString::fromStdString(cfg.root_cert_path));
        }
        if (parsed->has_timeout) {
            server_page_->timeout_spin()->setValue(static_cast<int>(cfg.connect_timeout_sec));
        }
        if (parsed->has_app_name) {
            server_page_->app_name_edit()->setText(QString::fromStdString(cfg.application_name));
        }
    }
    syncing_form_ = false;
    update_engine_specific_ui(false);
    set_status_text(
        QStringLiteral("Imported settings from connection string."),
        "#2E8B57",
        StatusIconState::Success);
    return true;
}

void ConnectionDialog::on_save_profile() {
    if (testing_) {
        return;
    }
    drivers::ConnectionConfig cfg;
    if (!try_build_config(cfg)) {
        return;
    }
    const QString pname = profile_name_edit_->text().trimmed();
    if (pname.isEmpty()) {
        set_status_text(
            QStringLiteral("Enter a profile name."),
            theme::kErrorTextFg,
            StatusIconState::Error);
        profile_name_edit_->setFocus();
        return;
    }

    std::string id = selected_profile_id_;
    if (id.empty()) {
        id = core::generate_uuid();
    }

    std::optional<std::string> group;
    const QString edited_group = profile_group_combo_->currentText().trimmed();
    if (!edited_group.isEmpty()) {
        group = edited_group.toStdString();
    } else {
        for (const auto& existing : profiles_) {
            if (existing.id == id && existing.group.has_value()) {
                group = existing.group;
                break;
            }
        }
    }

    const auto& desc = drivers::driver_ui_descriptor(cfg.engine);
    core::ConnectionProfile profile;
    profile.id       = id;
    profile.name     = pname.toStdString();
    profile.engine   = std::string(drivers::engine_kind_to_string(cfg.engine));
    profile.host     = cfg.host;
    profile.port     = static_cast<int>(cfg.port);
    profile.database = cfg.database;
    profile.username = cfg.username;
    profile.auth_mode = std::string(drivers::auth_kind_to_string(cfg.auth));
    profile.ssl_mode  = std::string(drivers::tls_mode_to_string(cfg.tls_mode));
    if (!cfg.root_cert_path.empty()) {
        profile.root_certificate = cfg.root_cert_path;
    }
    profile.group                 = std::move(group);
    profile.disable_query_history = disable_query_history();

    const bool remember_pwd = (desc.storage_kind == drivers::ConnectionStorageKind::NetworkServer) &&
                              server_page_->remember_password_check()->isChecked();
    profile.set_password(cfg.password, remember_pwd);

    std::string err;
    if (!core::save_connection_profile(state_dir_ / "connections.json", profile, &err)) {
        set_status_text(
            QStringLiteral("Profile was not saved. ") + QString::fromStdString(err),
            theme::kErrorTextFg,
            StatusIconState::Error);
        return;
    }

    const QString session_pwd = server_page_->password_edit()->text();
    load_profiles(profile.id);
    if (desc.storage_kind == drivers::ConnectionStorageKind::NetworkServer) {
        server_page_->password_edit()->setText(session_pwd);
        server_page_->remember_password_check()->setChecked(remember_pwd);
    }
    set_status_text(
        QStringLiteral("Profile saved."),
        "#2E8B57",
        StatusIconState::Success);
}

void ConnectionDialog::on_delete_profile() {
    if (testing_ || selected_profile_id_.empty()) {
        return;
    }
    const bool confirmed = confirm_delete_fn_
        ? confirm_delete_fn_()
        : (QMessageBox::question(
               this,
               QStringLiteral("Delete Profile"),
               QStringLiteral("Delete this saved profile and its protected password?"),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) == QMessageBox::Yes);
    if (!confirmed) {
        return;
    }

    std::string err;
    if (!core::remove_connection_profile(state_dir_ / "connections.json", selected_profile_id_, &err)) {
        set_status_text(
            QStringLiteral("Profile was not deleted. ") + QString::fromStdString(err),
            theme::kErrorTextFg,
            StatusIconState::Error);
        return;
    }

    load_profiles();
    on_new_profile();
    set_status_text(
        QStringLiteral("Profile deleted."),
        "#2E8B57",
        StatusIconState::Success);
}

void ConnectionDialog::on_accept_connect() {
    if (testing_) {
        return;
    }
    drivers::ConnectionConfig cfg;
    if (!try_build_config(cfg)) {
        return;
    }
    result_config_ = std::move(cfg);
    display_name_  = QString::fromStdString(result_config_.display_server());
    save_dialog_preferences(selected_profile_id_.empty() ? nullptr : &selected_profile_id_);
    accept();
}

bool ConnectionDialog::try_build_config(drivers::ConnectionConfig& out) {
    QString err_msg;
    QWidget* focus_widget = nullptr;
    const auto& desc = drivers::driver_ui_descriptor(current_engine_);

    const bool ok = (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile)
        ? file_page_->try_build_config(current_engine_, out, err_msg, focus_widget)
        : server_page_->try_build_config(current_engine_, out, err_msg, focus_widget);

    if (!ok) {
        set_status_text(err_msg, theme::kErrorTextFg, StatusIconState::Error);
        if (focus_widget != nullptr) {
            focus_widget->setFocus();
            if (auto* le = qobject_cast<QLineEdit*>(focus_widget)) {
                le->selectAll();
            }
        }
        return false;
    }
    return true;
}

void ConnectionDialog::start_test_connection() {
    if (testing_) {
        return;
    }
    drivers::ConnectionConfig cfg;
    if (!try_build_config(cfg)) {
        return;
    }

    if (worker_pool_ == nullptr && !owned_pool_) {
        owned_pool_ = std::make_unique<core::CatalogWorkerPool>(1);
    }
    core::CatalogWorkerPool* pool = worker_pool_ ? worker_pool_ : owned_pool_.get();

    test_canceled_user_ = false;
    test_cancel_ = std::make_shared<core::CancelToken>();
    const uint64_t gen = ++test_generation_;
    set_testing_ui(true);
    cancel_test_btn_->setFocus();
    set_status_text(
        QStringLiteral("Testing connection..."),
        "#696969",
        StatusIconState::Running);

    QPointer<ConnectionDialog> self(this);
    std::shared_ptr<core::CancelToken> token = test_cancel_;

    pool->submit([self, cfg = std::move(cfg), token, gen]() {
        std::expected<drivers::ConnectionTestResult, drivers::DbError> res;
        {
            auto drv = drivers::create_db_driver(cfg.engine);
            if (drv != nullptr) {
                res = drv->test_connection(cfg, *token);
            } else {
                res = std::unexpected(drivers::DbError{.message = "Driver plugin module could not be loaded."});
            }
        }
        // Release unused driver module when test completes so zero driver DLLs remain pinned if idle.
        (void)drivers::unload_unused_driver_modules(std::chrono::milliseconds{0});

        QMetaObject::invokeMethod(
            self,
            [self, res = std::move(res), gen, engine = cfg.engine]() {
                if (!self || gen != self->test_generation_) {
                    return;
                }
                self->set_testing_ui(false);
                if (res.has_value()) {
                    const auto& desc = drivers::driver_ui_descriptor(engine);
                    QString text;
                    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
                        text = QStringLiteral("Connection succeeded — %1 (%2)")
                            .arg(QString::fromStdString(res->version))
                            .arg(QString::fromStdString(res->database));
                    } else {
                        text = QStringLiteral("Connection succeeded.\nDatabase: %1   User: %2   TLS: %3\n%4")
                            .arg(QString::fromStdString(res->database))
                            .arg(QString::fromStdString(res->user))
                            .arg(res->tls_active ? QStringLiteral("Yes") : QStringLiteral("No"))
                            .arg(QString::fromStdString(res->version));
                    }
                    self->set_status_text(text, "#2E8B57", StatusIconState::Success);
                } else {
                    const auto& err = res.error();
                    if (self->test_canceled_user_ || err.is_canceled) {
                        self->set_status_text(
                            QStringLiteral("Connection test canceled. You can edit settings or test again."),
                            theme::kErrorTextFg,
                            StatusIconState::Error);
                    } else if (!err.sql_state.empty()) {
                        self->set_status_text(
                            QStringLiteral("Connection failed - %1: %2")
                                .arg(QString::fromStdString(err.sql_state))
                                .arg(QString::fromStdString(err.message)),
                            theme::kErrorTextFg,
                            StatusIconState::Error);
                    } else {
                        self->set_status_text(
                            QStringLiteral("Connection failed: %1").arg(QString::fromStdString(err.message)),
                            theme::kErrorTextFg,
                            StatusIconState::Error);
                    }
                }
                if (self->close_after_test_) {
                    self->close_after_test_ = false;
                    self->close();
                }
            },
            Qt::QueuedConnection);
    });
}

void ConnectionDialog::cancel_test_connection() {
    if (!testing_ || !test_cancel_) {
        return;
    }
    test_canceled_user_ = true;
    cancel_test_btn_->setEnabled(false);
    set_status_text(
        QStringLiteral("Canceling connection test... Waiting for the operation to finish."),
        "#696969",
        StatusIconState::Running);
    test_cancel_->cancel();
}

void ConnectionDialog::set_testing_ui(bool testing) {
    testing_ = testing;
    sidebar_->set_controls_enabled(!testing, !selected_profile_id_.empty());
    for (QToolButton* btn : engine_buttons_) {
        if (btn != nullptr) {
            btn->setEnabled(!testing);
        }
    }
    profile_name_edit_->setEnabled(!testing);
    profile_group_combo_->setEnabled(!testing);
    server_page_->set_controls_enabled(!testing);
    file_page_->set_controls_enabled(!testing);

    save_profile_btn_->setEnabled(!testing);
    test_btn_->setEnabled(!testing);
    connect_btn_->setEnabled(!testing);
    cancel_test_btn_->setVisible(testing);
    cancel_test_btn_->setEnabled(testing);

    if (!testing) {
        update_engine_specific_ui(false);
    }
}

void ConnectionDialog::set_status_text(
    const QString& text,
    const char* hex_color,
    StatusIconState icon_state) {
    status_label_->setText(text);
    status_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(hex_color)));

    if (text.isEmpty() || icon_state == StatusIconState::None) {
        status_icon_label_->setVisible(false);
        return;
    }

    IconId id = IconId::StatusIdle;
    switch (icon_state) {
    case StatusIconState::Running: id = IconId::StatusRunning; break;
    case StatusIconState::Success: id = IconId::StatusSuccess; break;
    case StatusIconState::Error:   id = IconId::StatusError;   break;
    case StatusIconState::None:    break;
    }
    status_icon_label_->setPixmap(render_icon_pixmap(id, 16, devicePixelRatioF()));
    status_icon_label_->setVisible(true);
}

void ConnectionDialog::save_dialog_preferences(const std::string* last_profile_id) {
    const std::filesystem::path prefs_path = state_dir_ / "connect-dialog.json";
    core::ConnectionDialogPreferences prefs = core::load_connection_dialog_preferences(prefs_path);
    if (last_profile_id != nullptr) {
        prefs.last_profile = *last_profile_id;
    }
    if (!isMinimized() && !isMaximized()) {
        prefs.width  = static_cast<double>(width());
        prefs.height = static_cast<double>(height());
    }
    (void)core::save_connection_dialog_preferences(prefs_path, prefs);
}

void ConnectionDialog::prefill(const drivers::ConnectionConfig& cfg) {
    on_new_profile();
    syncing_form_ = true;
    current_engine_ = cfg.engine;
    const size_t eng_idx = engine_to_idx(cfg.engine);
    engine_combo_->setCurrentIndex(static_cast<int>(eng_idx));
    if (engine_buttons_[eng_idx] != nullptr) {
        engine_buttons_[eng_idx]->setChecked(true);
    }

    const auto& desc = drivers::driver_ui_descriptor(cfg.engine);
    if (desc.storage_kind == drivers::ConnectionStorageKind::LocalFile) {
        profile_name_edit_->setText(QString::fromStdString(cfg.display_server()));
        file_page_->configure_for_engine(desc);
        file_page_->apply_database_value(QString::fromStdString(cfg.database), false);
    } else {
        profile_name_edit_->setText(
            QStringLiteral("%1:%2").arg(QString::fromStdString(cfg.host)).arg(cfg.port));
        server_page_->host_edit()->setText(QString::fromStdString(cfg.host));
        server_page_->port_edit()->setText(QString::number(cfg.port));
        server_page_->database_edit()->setText(QString::fromStdString(cfg.database));
        server_page_->auth_combo()->setCurrentIndex(
            cfg.auth == drivers::AuthKind::IntegratedWindowsOrKerberos ? 1 : 0);
        server_page_->username_edit()->setText(QString::fromStdString(cfg.username));
        server_page_->ssl_combo()->setCurrentText(
            QString::fromStdString(std::string(drivers::tls_mode_to_string(cfg.tls_mode))));
        server_page_->root_cert_edit()->setText(QString::fromStdString(cfg.root_cert_path));
        server_page_->timeout_spin()->setValue(static_cast<int>(cfg.connect_timeout_sec));
        server_page_->app_name_edit()->setText(QString::fromStdString(cfg.application_name));
    }
    syncing_form_ = false;
    update_engine_specific_ui(false);
    set_status_text(
        QStringLiteral("New server registration from Object Explorer. Enter the password to save it, then choose Save Profile."),
        theme::kMutedTextFg);
}

void ConnectionDialog::select_profile_by_id(const QString& profile_id) {
    const std::string want = profile_id.toStdString();
    sidebar_->select_profile_by_id(want);
    on_profile_selected_by_id(profile_id);
}

void ConnectionDialog::closeEvent(QCloseEvent* event) {
    if (testing_) {
        event->ignore();
        close_after_test_ = true;
        cancel_test_connection();
        return;
    }
    server_page_->clear_secrets();
    save_dialog_preferences(nullptr);
    QDialog::closeEvent(event);
}

void ConnectionDialog::reject() {
    if (testing_) {
        close_after_test_ = true;
        cancel_test_connection();
        return;
    }
    server_page_->clear_secrets();
    save_dialog_preferences(nullptr);
    QDialog::reject();
}

} // namespace capybaradb::ui
