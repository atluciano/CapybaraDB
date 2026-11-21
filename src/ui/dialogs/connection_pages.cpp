#include "ui/dialogs/connection_pages.hpp"

#include "ui/shell/icon_catalog.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace capybaradb::ui {
namespace {

QIcon make_eye_icon(bool visible) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(0x29, 0x4A, 0x60), 1.4);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(8.0, 8.0), 6.0, 3.6);
    p.setBrush(QColor(0x29, 0x4A, 0x60));
    p.drawEllipse(QPointF(8.0, 8.0), 1.8, 1.8);
    if (!visible) {
        p.setPen(QPen(QColor(0xA4, 0x26, 0x2C), 1.5));
        p.drawLine(QPointF(3.0, 13.0), QPointF(13.0, 3.0));
    }
    return QIcon(pm);
}

bool is_default_server_db(const QString& text) {
    return text.isEmpty() ||
           text == QLatin1String("postgres") ||
           text == QLatin1String("master") ||
           text == QLatin1String(":memory:");
}

bool is_default_server_port(const QString& text) {
    return text.isEmpty() ||
           text == QLatin1String("5432") ||
           text == QLatin1String("1433") ||
           text == QLatin1String("0");
}

bool is_default_server_user(const QString& text) {
    return text.isEmpty() ||
           text == QLatin1String("postgres") ||
           text == QLatin1String("sa") ||
           text == QLatin1String("sqlite");
}

} // namespace

// ===========================================================================
// FileConnectionPage
// ===========================================================================

FileConnectionPage::FileConnectionPage(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("FileConnectionPage"));
    setAcceptDrops(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(10);

    // 1. Mode row (Existing file / Create new / In-memory)
    auto* mode_box = new QWidget(this);
    auto* mode_row = new QHBoxLayout(mode_box);
    mode_row->setContentsMargins(0, 0, 0, 0);
    mode_row->setSpacing(14);

    existing_radio_   = new QRadioButton(QStringLiteral("Existing &file"), mode_box);
    create_new_radio_ = new QRadioButton(QStringLiteral("Create &new file"), mode_box);
    in_memory_radio_  = new QRadioButton(QStringLiteral("In-memory (:&memory:)"), mode_box);
    existing_radio_->setChecked(true);

    mode_row->addWidget(existing_radio_);
    mode_row->addWidget(create_new_radio_);
    mode_row->addWidget(in_memory_radio_);
    mode_row->addStretch(1);

    form_layout_ = new QFormLayout();
    form_layout_->setContentsMargins(0, 0, 0, 0);
    form_layout_->setHorizontalSpacing(10);
    form_layout_->setVerticalSpacing(10);
    form_layout_->addRow(QStringLiteral("Connection mode:"), mode_box);

    // 2. File path row with magnifying glass / folder browse button
    path_row_widget_ = new QWidget(this);
    auto* path_row = new QHBoxLayout(path_row_widget_);
    path_row->setContentsMargins(0, 0, 0, 0);
    path_row->setSpacing(6);

    file_path_edit_ = new QLineEdit(path_row_widget_);
    file_path_edit_->setAccessibleName(QStringLiteral("Database file path"));
    file_path_edit_->setPlaceholderText(QStringLiteral("Select or drag & drop a .db / .sqlite file..."));
    file_path_edit_->setMinimumHeight(25);
    file_path_edit_->setClearButtonEnabled(true);

    browse_btn_ = new QPushButton(get_icon(IconId::BrowseFile), QStringLiteral("Browse…"), path_row_widget_);
    browse_btn_->setAccessibleName(QStringLiteral("Browse database file"));
    browse_btn_->setToolTip(QStringLiteral("Browse for a local SQLite database file"));
    browse_btn_->setAutoDefault(false);
    browse_btn_->setMinimumHeight(25);

    path_row->addWidget(file_path_edit_, 1);
    path_row->addWidget(browse_btn_);
    form_layout_->addRow(QStringLiteral("Database &file:"), path_row_widget_);

    root->addLayout(form_layout_);

    // 3. Compact options & security note
    disable_history_check_ = new QCheckBox(
        QStringLiteral("Do not record query &history for this connection"),
        this);
    disable_history_check_->setToolTip(
        QStringLiteral("Saved with the profile. Query History then records nothing that runs on this database."));
    root->addWidget(disable_history_check_);

    note_label_ = new QLabel(
        QStringLiteral("Local database files do not require a server host, port, user password, or TLS certificate. "
                       "You can also drag and drop a .db or .sqlite file directly onto this page."),
        this);
    note_label_->setObjectName(QStringLiteral("DialogMutedText"));
    note_label_->setWordWrap(true);
    root->addWidget(note_label_);
    root->addStretch(1);

    connect(existing_radio_, &QRadioButton::toggled, this, [this](bool) { on_mode_toggled(); });
    connect(create_new_radio_, &QRadioButton::toggled, this, [this](bool) { on_mode_toggled(); });
    connect(in_memory_radio_, &QRadioButton::toggled, this, [this](bool) { on_mode_toggled(); });
    connect(browse_btn_, &QPushButton::clicked, this, &FileConnectionPage::on_browse_clicked);
    connect(file_path_edit_, &QLineEdit::textEdited, this, [this](const QString&) {
        emit database_edited();
    });
    connect(disable_history_check_, &QCheckBox::toggled, this, &FileConnectionPage::disable_history_toggled);

    configure_for_engine(drivers::driver_ui_descriptor(drivers::EngineKind::Sqlite));
}

void FileConnectionPage::configure_for_engine(const drivers::DriverUiDescriptor& desc) {
    current_engine_ = desc.engine;
    file_filter_    = QString::fromUtf8(desc.file_filter.data(), static_cast<qsizetype>(desc.file_filter.size()));
    default_ext_    = QString::fromUtf8(desc.default_file_ext.data(), static_cast<qsizetype>(desc.default_file_ext.size()));
    in_memory_radio_->setVisible(desc.supports_in_memory);
    on_mode_toggled();
}

void FileConnectionPage::reset_for_new_profile(const drivers::DriverUiDescriptor& desc) {
    configure_for_engine(desc);
    file_path_edit_->clear();
    disable_history_check_->setChecked(false);
    existing_radio_->setChecked(true);
    on_mode_toggled();
}

void FileConnectionPage::apply_database_value(const QString& database, bool disable_history) {
    disable_history_check_->setChecked(disable_history);
    const QString trimmed = database.trimmed();
    if (trimmed == QLatin1String(":memory:")) {
        in_memory_radio_->setChecked(true);
        file_path_edit_->clear();
    } else if (trimmed.isEmpty() || is_default_server_db(trimmed)) {
        existing_radio_->setChecked(true);
        file_path_edit_->clear();
    } else {
        const QFileInfo fi(trimmed);
        if (fi.exists() || !create_new_radio_->isChecked()) {
            existing_radio_->setChecked(true);
        }
        file_path_edit_->setText(trimmed);
    }
    on_mode_toggled();
}

QString FileConnectionPage::current_database_value() const {
    if (in_memory_radio_->isChecked()) {
        return QStringLiteral(":memory:");
    }
    return file_path_edit_->text().trimmed();
}

void FileConnectionPage::set_file_mode(FileMode mode) {
    switch (mode) {
    case FileMode::ExistingFile:
        existing_radio_->setChecked(true);
        break;
    case FileMode::CreateNew:
        create_new_radio_->setChecked(true);
        break;
    case FileMode::InMemory:
        in_memory_radio_->setChecked(true);
        break;
    }
    on_mode_toggled();
}

FileConnectionPage::FileMode FileConnectionPage::file_mode() const noexcept {
    if (in_memory_radio_->isChecked()) {
        return FileMode::InMemory;
    }
    if (create_new_radio_->isChecked()) {
        return FileMode::CreateNew;
    }
    return FileMode::ExistingFile;
}

void FileConnectionPage::on_mode_toggled() {
    const bool is_mem = in_memory_radio_->isChecked();
    form_layout_->setRowVisible(path_row_widget_, !is_mem);
    if (create_new_radio_->isChecked()) {
        file_path_edit_->setPlaceholderText(QStringLiteral("Choose where to create the new .db / .sqlite file..."));
    } else {
        file_path_edit_->setPlaceholderText(QStringLiteral("Select or drag & drop an existing .db / .sqlite file..."));
    }
}

void FileConnectionPage::on_browse_clicked() {
    const bool save_mode = create_new_radio_->isChecked();
    const QString filter = file_filter_.isEmpty()
        ? QStringLiteral("SQLite Databases (*.db *.sqlite *.sqlite3);;All Files (*.*)")
        : file_filter_;

    QString chosen;
    if (file_picker_) {
        chosen = file_picker_(save_mode, filter);
    } else if (save_mode) {
        chosen = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Create SQLite Database File"),
            file_path_edit_->text().trimmed(),
            filter);
    } else {
        chosen = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Open SQLite Database File"),
            file_path_edit_->text().trimmed(),
            filter);
    }

    if (!chosen.isEmpty()) {
        if (save_mode && !default_ext_.isEmpty() && QFileInfo(chosen).suffix().isEmpty()) {
            chosen += default_ext_;
        }
        file_path_edit_->setText(QDir::toNativeSeparators(chosen));
        emit database_edited();
    }
}

bool FileConnectionPage::try_build_config(
    drivers::EngineKind engine,
    drivers::ConnectionConfig& out,
    QString& error_out,
    QWidget*& focus_out) {
    if (in_memory_radio_->isChecked()) {
        out = drivers::ConnectionConfig{};
        out.engine              = engine;
        out.host                = "localhost";
        out.port                = 0;
        out.database            = ":memory:";
        out.username            = "sqlite";
        out.application_name    = "CapybaraDB";
        out.connect_timeout_sec = 5;
        return true;
    }

    const QString raw_path = file_path_edit_->text().trimmed();
    if (raw_path.isEmpty()) {
        error_out = QStringLiteral("Select a database file path or choose In-memory (:memory:).");
        focus_out = file_path_edit_;
        return false;
    }

    if (raw_path != QLatin1String(":memory:") && !raw_path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QFileInfo fi(raw_path);
        if (existing_radio_->isChecked()) {
            if (!fi.exists() || !fi.isFile()) {
                error_out = QStringLiteral("The selected database file does not exist. Switch to \"Create new file\" to create it.");
                focus_out = file_path_edit_;
                return false;
            }
        } else if (create_new_radio_->isChecked()) {
            const QDir parent_dir = fi.dir();
            if (!parent_dir.exists()) {
                error_out = QStringLiteral("The destination folder for the new database file does not exist.");
                focus_out = file_path_edit_;
                return false;
            }
        }
    }

    out = drivers::ConnectionConfig{};
    out.engine              = engine;
    out.host                = "localhost";
    out.port                = 0;
    out.database            = raw_path.toStdString();
    out.username            = "sqlite";
    out.application_name    = "CapybaraDB";
    out.connect_timeout_sec = 5;
    return true;
}

void FileConnectionPage::set_controls_enabled(bool enabled) {
    controls_enabled_ = enabled;
    existing_radio_->setEnabled(enabled);
    create_new_radio_->setEnabled(enabled);
    in_memory_radio_->setEnabled(enabled);
    file_path_edit_->setEnabled(enabled);
    browse_btn_->setEnabled(enabled);
    disable_history_check_->setEnabled(enabled);
}

void FileConnectionPage::dragEnterEvent(QDragEnterEvent* event) {
    if (controls_enabled_ && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dragEnterEvent(event);
}

void FileConnectionPage::dropEvent(QDropEvent* event) {
    if (!controls_enabled_ || event->mimeData() == nullptr || !event->mimeData()->hasUrls()) {
        QWidget::dropEvent(event);
        return;
    }
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            const QString local = QDir::toNativeSeparators(url.toLocalFile());
            if (!local.isEmpty()) {
                if (in_memory_radio_->isChecked()) {
                    existing_radio_->setChecked(true);
                }
                file_path_edit_->setText(local);
                emit database_edited();
                event->acceptProposedAction();
                return;
            }
        }
    }
}

// ===========================================================================
// ServerConnectionPage
// ===========================================================================

ServerConnectionPage::ServerConnectionPage(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("ServerConnectionPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("ConnectionInnerTabs"));

    // --- Tab 1: General ---
    general_tab_ = new QWidget(tabs_);
    auto* gen_outer = new QVBoxLayout(general_tab_);
    gen_outer->setContentsMargins(12, 10, 12, 10);
    gen_outer->setSpacing(8);

    general_form_ = new QFormLayout();
    general_form_->setContentsMargins(0, 0, 0, 0);
    general_form_->setHorizontalSpacing(10);
    general_form_->setVerticalSpacing(8);

    // Host + Port on the same line (proportion 3:1)
    auto* host_port_w = new QWidget(general_tab_);
    auto* hp_row = new QHBoxLayout(host_port_w);
    hp_row->setContentsMargins(0, 0, 0, 0);
    hp_row->setSpacing(6);

    host_edit_ = new QLineEdit(QStringLiteral("localhost"), host_port_w);
    host_edit_->setAccessibleName(QStringLiteral("Server name"));
    host_edit_->setPlaceholderText(QStringLiteral("localhost or host.domain"));
    host_edit_->setMinimumHeight(25);

    auto* port_lbl = new QLabel(QStringLiteral("&Port:"), host_port_w);
    port_edit_ = new QLineEdit(QStringLiteral("5432"), host_port_w);
    port_edit_->setAccessibleName(QStringLiteral("Port"));
    port_edit_->setValidator(new QIntValidator(1, 65535, port_edit_));
    port_edit_->setMinimumHeight(25);
    port_edit_->setFixedWidth(74);
    port_lbl->setBuddy(port_edit_);

    hp_row->addWidget(host_edit_, 3);
    hp_row->addWidget(port_lbl);
    hp_row->addWidget(port_edit_, 1);
    general_form_->addRow(QStringLiteral("&Server name:"), host_port_w);

    database_edit_ = new QLineEdit(QStringLiteral("postgres"), general_tab_);
    database_edit_->setAccessibleName(QStringLiteral("Database"));
    database_edit_->setMinimumHeight(25);
    general_form_->addRow(QStringLiteral("&Database:"), database_edit_);

    auth_combo_ = new QComboBox(general_tab_);
    auth_combo_->setAccessibleName(QStringLiteral("Authentication"));
    auth_combo_->addItem(QStringLiteral("SQL / Password"), static_cast<int>(drivers::AuthKind::Password));
    auth_combo_->addItem(QStringLiteral("Windows / Kerberos"), static_cast<int>(drivers::AuthKind::IntegratedWindowsOrKerberos));
    auth_combo_->setMinimumHeight(25);
    general_form_->addRow(QStringLiteral("&Authentication:"), auth_combo_);

    username_edit_ = new QLineEdit(QStringLiteral("postgres"), general_tab_);
    username_edit_->setAccessibleName(QStringLiteral("User name"));
    username_edit_->setMinimumHeight(25);
    general_form_->addRow(QStringLiteral("&User name:"), username_edit_);

    password_edit_ = new QLineEdit(general_tab_);
    password_edit_->setAccessibleName(QStringLiteral("Password"));
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setMinimumHeight(25);
    toggle_pwd_action_ = password_edit_->addAction(
        make_eye_icon(false),
        QLineEdit::TrailingPosition);
    toggle_pwd_action_->setToolTip(QStringLiteral("Show or hide password"));
    connect(toggle_pwd_action_, &QAction::triggered, this, &ServerConnectionPage::on_toggle_password_visibility);
    general_form_->addRow(QStringLiteral("Pass&word:"), password_edit_);

    // Remember password + Paste URL inline row
    auto* pwd_opts_w = new QWidget(general_tab_);
    auto* pwd_opts_row = new QHBoxLayout(pwd_opts_w);
    pwd_opts_row->setContentsMargins(0, 0, 0, 0);
    pwd_opts_row->setSpacing(8);

#ifdef _WIN32
    remember_check_ = new QCheckBox(QStringLiteral("Save password for this Windows user (&M)"), pwd_opts_w);
#else
    remember_check_ = new QCheckBox(QStringLiteral("Save password for this user (&M)"), pwd_opts_w);
#endif
    paste_url_btn_ = new QPushButton(QStringLiteral("Paste &URL…"), pwd_opts_w);
    paste_url_btn_->setAccessibleName(QStringLiteral("Paste connection URL"));
    paste_url_btn_->setToolTip(QStringLiteral("Fill fields from clipboard connection URL or connection string"));
    paste_url_btn_->setAutoDefault(false);

    pwd_opts_row->addWidget(remember_check_);
    pwd_opts_row->addStretch(1);
    pwd_opts_row->addWidget(paste_url_btn_);
    general_form_->addRow(QString{}, pwd_opts_w);

    gen_outer->addLayout(general_form_);
    gen_outer->addStretch(1);
    tabs_->addTab(general_tab_, QStringLiteral("&General"));

    // --- Tab 2: Security / TLS ---
    security_tab_ = new QWidget(tabs_);
    auto* sec_outer = new QVBoxLayout(security_tab_);
    sec_outer->setContentsMargins(12, 10, 12, 10);
    sec_outer->setSpacing(8);

    security_form_ = new QFormLayout();
    security_form_->setContentsMargins(0, 0, 0, 0);
    security_form_->setHorizontalSpacing(10);
    security_form_->setVerticalSpacing(8);

    ssl_combo_ = new QComboBox(security_tab_);
    ssl_combo_->setAccessibleName(QStringLiteral("SSL mode"));
    ssl_combo_->addItems({
        QStringLiteral("VerifyFull"),
        QStringLiteral("Require"),
        QStringLiteral("Prefer"),
        QStringLiteral("Disable"),
    });
    ssl_combo_->setMinimumHeight(25);
    security_form_->addRow(QStringLiteral("TLS / SS&L mode:"), ssl_combo_);

    cert_row_widget_ = new QWidget(security_tab_);
    auto* cert_row = new QHBoxLayout(cert_row_widget_);
    cert_row->setContentsMargins(0, 0, 0, 0);
    cert_row->setSpacing(6);
    root_cert_edit_ = new QLineEdit(cert_row_widget_);
    root_cert_edit_->setAccessibleName(QStringLiteral("Root certificate"));
    root_cert_edit_->setMinimumHeight(25);
    root_cert_edit_->setPlaceholderText(QStringLiteral("Optional PEM file (system trust store if empty)"));
    root_cert_edit_->setToolTip(QStringLiteral(
        "Optional PEM file trusted for VerifyFull, such as a self-signed server certificate. "
        "Empty uses the system trust store."));
    browse_cert_btn_ = new QPushButton(get_icon(IconId::BrowseFile), QStringLiteral("…"), cert_row_widget_);
    browse_cert_btn_->setAccessibleName(QStringLiteral("Browse root certificate"));
    browse_cert_btn_->setFixedWidth(36);
    browse_cert_btn_->setAutoDefault(false);
    cert_row->addWidget(root_cert_edit_, 1);
    cert_row->addWidget(browse_cert_btn_);
    security_form_->addRow(QStringLiteral("Root certificate:"), cert_row_widget_);

    sec_outer->addLayout(security_form_);
    auto* tls_note = new QLabel(
        QStringLiteral("VerifyFull validates server identity and certificate chain. Use Require or Prefer for self-signed development servers without a custom CA."),
        security_tab_);
    tls_note->setObjectName(QStringLiteral("DialogMutedText"));
    tls_note->setWordWrap(true);
    sec_outer->addWidget(tls_note);
    sec_outer->addStretch(1);
    tabs_->addTab(security_tab_, QStringLiteral("Security / &TLS"));

    // --- Tab 3: Advanced ---
    advanced_tab_ = new QWidget(tabs_);
    auto* adv_outer = new QVBoxLayout(advanced_tab_);
    adv_outer->setContentsMargins(12, 10, 12, 10);
    adv_outer->setSpacing(8);

    advanced_form_ = new QFormLayout();
    advanced_form_->setContentsMargins(0, 0, 0, 0);
    advanced_form_->setHorizontalSpacing(10);
    advanced_form_->setVerticalSpacing(8);

    timeout_spin_ = new QSpinBox(advanced_tab_);
    timeout_spin_->setAccessibleName(QStringLiteral("Connect timeout"));
    timeout_spin_->setRange(1, 300);
    timeout_spin_->setValue(10);
    timeout_spin_->setSuffix(QStringLiteral(" s"));
    timeout_spin_->setMinimumHeight(25);
    advanced_form_->addRow(QStringLiteral("Connect &timeout:"), timeout_spin_);

    app_name_edit_ = new QLineEdit(QStringLiteral("CapybaraDB"), advanced_tab_);
    app_name_edit_->setAccessibleName(QStringLiteral("Application name"));
    app_name_edit_->setMinimumHeight(25);
    advanced_form_->addRow(QStringLiteral("&Application name:"), app_name_edit_);

    adv_outer->addLayout(advanced_form_);

    disable_history_check_ = new QCheckBox(
        QStringLiteral("Do not record query &history for this server"),
        advanced_tab_);
    disable_history_check_->setToolTip(
        QStringLiteral("Saved with the profile. Query History then records nothing that runs on this server and port."));
    adv_outer->addWidget(disable_history_check_);

#ifdef _WIN32
    auto* sec_note = new QLabel(
        QStringLiteral("Save Profile stores settings locally. Password saving is optional and protected by Windows DPAPI. Connect does not save changes."),
        advanced_tab_);
#else
    auto* sec_note = new QLabel(
        QStringLiteral("Save Profile stores settings locally. Password saving is optional and protected via libsecret/local keyring. Connect does not save changes."),
        advanced_tab_);
#endif
    sec_note->setObjectName(QStringLiteral("DialogMutedText"));
    sec_note->setWordWrap(true);
    adv_outer->addWidget(sec_note);
    adv_outer->addStretch(1);
    tabs_->addTab(advanced_tab_, QStringLiteral("Ad&vanced"));

    root->addWidget(tabs_);

    connect(port_edit_, &QLineEdit::textEdited, this, [this]() { port_edited_ = true; });
    connect(database_edit_, &QLineEdit::textEdited, this, [this]() { db_edited_ = true; });
    connect(username_edit_, &QLineEdit::textEdited, this, [this]() { user_edited_ = true; });
    connect(auth_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        update_row_visibility();
        emit auth_mode_changed();
    });
    connect(browse_cert_btn_, &QPushButton::clicked, this, &ServerConnectionPage::on_browse_root_cert);
    connect(paste_url_btn_, &QPushButton::clicked, this, &ServerConnectionPage::paste_url_requested);
    connect(disable_history_check_, &QCheckBox::toggled, this, &ServerConnectionPage::disable_history_toggled);

    configure_for_engine(current_desc_, false);
}

void ServerConnectionPage::configure_for_engine(
    const drivers::DriverUiDescriptor& desc,
    bool apply_smart_defaults) {
    current_desc_ = desc;

    if (apply_smart_defaults) {
        if (!port_edited_ || is_default_server_port(port_edit_->text())) {
            port_edit_->setText(QString::number(desc.default_port));
        }
        if (!db_edited_ || is_default_server_db(database_edit_->text())) {
            database_edit_->setText(QString::fromUtf8(
                desc.default_database.data(),
                static_cast<qsizetype>(desc.default_database.size())));
        }
        if (!user_edited_ || is_default_server_user(username_edit_->text())) {
            username_edit_->setText(QString::fromUtf8(
                desc.default_username.data(),
                static_cast<qsizetype>(desc.default_username.size())));
        }
    }

    if (!desc.supports_integrated_auth) {
        auth_combo_->setCurrentIndex(0);
    }

    update_row_visibility();
}

void ServerConnectionPage::reset_for_new_profile(const drivers::DriverUiDescriptor& desc) {
    clear_secrets();
    remember_check_->setChecked(false);
    disable_history_check_->setChecked(false);
    port_edited_ = false;
    db_edited_   = false;
    user_edited_ = false;
    configure_for_engine(desc, true);
}

void ServerConnectionPage::clear_secrets() {
    QString cur = password_edit_->text();
    for (qsizetype i = 0; i < cur.size(); ++i) {
        cur[i] = QChar(0);
    }
    password_edit_->clear();
    if (password_visible_) {
        password_visible_ = false;
        password_edit_->setEchoMode(QLineEdit::Password);
        toggle_pwd_action_->setIcon(make_eye_icon(false));
    }
}

void ServerConnectionPage::mark_fields_unedited() {
    port_edited_ = false;
    db_edited_   = false;
    user_edited_ = false;
}

void ServerConnectionPage::update_row_visibility() {
    // 1. Authentication row: only visible when engine supports integrated auth (e.g. SQL Server)
    general_form_->setRowVisible(auth_combo_, current_desc_.supports_integrated_auth);

    // 2. Username, Password, and Save Password rows: hidden when Integrated auth is active
    const auto auth = current_desc_.supports_integrated_auth
        ? static_cast<drivers::AuthKind>(auth_combo_->currentData().toInt())
        : drivers::AuthKind::Password;
    const bool use_creds = (auth == drivers::AuthKind::Password);

    general_form_->setRowVisible(username_edit_, use_creds);
    general_form_->setRowVisible(password_edit_, use_creds);
    remember_check_->setVisible(use_creds);

    // Also update enabled state for tests or keyboard navigation
    auth_combo_->setEnabled(controls_enabled_ && current_desc_.supports_integrated_auth);
    username_edit_->setEnabled(controls_enabled_ && use_creds);
    password_edit_->setEnabled(controls_enabled_ && use_creds);
    remember_check_->setEnabled(controls_enabled_ && use_creds);

    // 3. Security / TLS tab and Root Certificate row
    tabs_->setTabVisible(1, current_desc_.supports_tls);
    security_form_->setRowVisible(cert_row_widget_, current_desc_.supports_root_cert);
    root_cert_edit_->setEnabled(controls_enabled_ && current_desc_.supports_root_cert);
    browse_cert_btn_->setEnabled(controls_enabled_ && current_desc_.supports_root_cert);
}

void ServerConnectionPage::on_browse_root_cert() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Root certificate"),
        root_cert_edit_->text().trimmed(),
        QStringLiteral("PEM certificates (*.crt *.pem *.cer);;All files (*.*)"));
    if (!path.isEmpty()) {
        root_cert_edit_->setText(path);
    }
}

void ServerConnectionPage::on_toggle_password_visibility() {
    password_visible_ = !password_visible_;
    password_edit_->setEchoMode(password_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    toggle_pwd_action_->setIcon(make_eye_icon(password_visible_));
}

bool ServerConnectionPage::try_build_config(
    drivers::EngineKind engine,
    drivers::ConnectionConfig& out,
    QString& error_out,
    QWidget*& focus_out) {
    const QString host = host_edit_->text().trimmed();
    if (host.isEmpty()) {
        tabs_->setCurrentIndex(0);
        error_out = QStringLiteral("Enter a server name.");
        focus_out = host_edit_;
        return false;
    }

    bool port_ok = false;
    const int port = port_edit_->text().trimmed().toInt(&port_ok);
    if (!port_ok || port < 1 || port > 65535) {
        tabs_->setCurrentIndex(0);
        error_out = QStringLiteral("Enter a port from 1 to 65535.");
        focus_out = port_edit_;
        return false;
    }

    const QString db = database_edit_->text().trimmed();
    if (db.isEmpty()) {
        tabs_->setCurrentIndex(0);
        error_out = QStringLiteral("Enter a database name.");
        focus_out = database_edit_;
        return false;
    }

    const auto& desc = drivers::driver_ui_descriptor(engine);
    const auto auth  = desc.supports_integrated_auth
        ? static_cast<drivers::AuthKind>(auth_combo_->currentData().toInt())
        : drivers::AuthKind::Password;

    const QString user = username_edit_->text().trimmed();
    if (auth == drivers::AuthKind::Password && user.isEmpty()) {
        tabs_->setCurrentIndex(0);
        error_out = QStringLiteral("Enter a user name.");
        focus_out = username_edit_;
        return false;
    }

    const QString root_cert = root_cert_edit_->text().trimmed();
    if (desc.supports_root_cert && !root_cert.isEmpty() && !QFileInfo::exists(root_cert)) {
        tabs_->setCurrentIndex(1);
        error_out = QStringLiteral("The root certificate file was not found.");
        focus_out = root_cert_edit_;
        return false;
    }

    const QString app_name = app_name_edit_->text().trimmed();

    out = drivers::ConnectionConfig{};
    out.engine              = engine;
    out.host                = host.toStdString();
    out.port                = static_cast<uint16_t>(port);
    out.database            = db.toStdString();
    out.username            = user.toStdString();
    out.password            = password_edit_->text().toStdString();
    out.auth                = auth;
    out.tls_mode            = drivers::tls_mode_from_string(ssl_combo_->currentText().toStdString());
    out.root_cert_path      = desc.supports_root_cert ? root_cert.toStdString() : std::string{};
    out.application_name    = app_name.isEmpty() ? std::string{"CapybaraDB"} : app_name.toStdString();
    out.connect_timeout_sec = static_cast<uint32_t>(timeout_spin_->value());
    return true;
}

void ServerConnectionPage::set_controls_enabled(bool enabled) {
    controls_enabled_ = enabled;
    host_edit_->setEnabled(enabled);
    port_edit_->setEnabled(enabled);
    database_edit_->setEnabled(enabled);
    paste_url_btn_->setEnabled(enabled);
    ssl_combo_->setEnabled(enabled);
    timeout_spin_->setEnabled(enabled);
    app_name_edit_->setEnabled(enabled);
    disable_history_check_->setEnabled(enabled);
    update_row_visibility();
}

} // namespace capybaradb::ui
