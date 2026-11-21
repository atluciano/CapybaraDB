#pragma once

#include "drivers/common/db_types.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"

#include <QWidget>
#include <functional>

class QAction;
class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

namespace capybaradb::ui {

class FileConnectionPage final : public QWidget {
    Q_OBJECT

public:
    enum class FileMode : uint8_t {
        ExistingFile = 0,
        CreateNew    = 1,
        InMemory     = 2,
    };

    explicit FileConnectionPage(QWidget* parent = nullptr);

    void configure_for_engine(const drivers::DriverUiDescriptor& desc);
    void reset_for_new_profile(const drivers::DriverUiDescriptor& desc);
    void apply_database_value(const QString& database, bool disable_history);
    [[nodiscard]] QString current_database_value() const;

    void set_file_mode(FileMode mode);
    [[nodiscard]] FileMode file_mode() const noexcept;

    [[nodiscard]] bool try_build_config(
        drivers::EngineKind engine,
        drivers::ConnectionConfig& out,
        QString& error_out,
        QWidget*& focus_out);

    void set_controls_enabled(bool enabled);

    // Accessors for testing and inspection
    [[nodiscard]] QRadioButton* existing_file_radio() const noexcept { return existing_radio_; }
    [[nodiscard]] QRadioButton* create_new_radio() const noexcept { return create_new_radio_; }
    [[nodiscard]] QRadioButton* in_memory_radio() const noexcept { return in_memory_radio_; }
    [[nodiscard]] QWidget* file_path_row() const noexcept { return path_row_widget_; }
    [[nodiscard]] QLineEdit* file_path_edit() const noexcept { return file_path_edit_; }
    [[nodiscard]] QPushButton* browse_button() const noexcept { return browse_btn_; }
    [[nodiscard]] QCheckBox* disable_history_check() const noexcept { return disable_history_check_; }

    void set_file_picker(std::function<QString(bool save_mode, const QString& filter)> picker) {
        file_picker_ = std::move(picker);
    }

signals:
    void database_edited();
    void disable_history_toggled(bool checked);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void on_mode_toggled();
    void on_browse_clicked();

    drivers::EngineKind current_engine_ = drivers::EngineKind::Sqlite;
    QString             file_filter_;
    QString             default_ext_;
    bool                controls_enabled_ = true;

    QRadioButton* existing_radio_        = nullptr;
    QRadioButton* create_new_radio_      = nullptr;
    QRadioButton* in_memory_radio_       = nullptr;
    QFormLayout*  form_layout_           = nullptr;
    QWidget*      path_row_widget_       = nullptr;
    QLineEdit*    file_path_edit_        = nullptr;
    QPushButton*  browse_btn_            = nullptr;
    QCheckBox*    disable_history_check_ = nullptr;
    QLabel*       note_label_            = nullptr;

    std::function<QString(bool save_mode, const QString& filter)> file_picker_;
};

class ServerConnectionPage final : public QWidget {
    Q_OBJECT

public:
    explicit ServerConnectionPage(QWidget* parent = nullptr);

    void configure_for_engine(const drivers::DriverUiDescriptor& desc, bool apply_smart_defaults);
    void reset_for_new_profile(const drivers::DriverUiDescriptor& desc);
    void clear_secrets();

    [[nodiscard]] bool try_build_config(
        drivers::EngineKind engine,
        drivers::ConnectionConfig& out,
        QString& error_out,
        QWidget*& focus_out);

    void set_controls_enabled(bool enabled);
    void mark_fields_unedited();

    // Accessors for testing and inspection
    [[nodiscard]] QTabWidget* tab_widget() const noexcept { return tabs_; }
    [[nodiscard]] QFormLayout* general_form_layout() const noexcept { return general_form_; }
    [[nodiscard]] QFormLayout* security_form_layout() const noexcept { return security_form_; }
    [[nodiscard]] QLineEdit* host_edit() const noexcept { return host_edit_; }
    [[nodiscard]] QLineEdit* port_edit() const noexcept { return port_edit_; }
    [[nodiscard]] QLineEdit* database_edit() const noexcept { return database_edit_; }
    [[nodiscard]] QComboBox* auth_combo() const noexcept { return auth_combo_; }
    [[nodiscard]] QLineEdit* username_edit() const noexcept { return username_edit_; }
    [[nodiscard]] QLineEdit* password_edit() const noexcept { return password_edit_; }
    [[nodiscard]] QAction* toggle_password_action() const noexcept { return toggle_pwd_action_; }
    [[nodiscard]] QCheckBox* remember_password_check() const noexcept { return remember_check_; }
    [[nodiscard]] QPushButton* paste_url_button() const noexcept { return paste_url_btn_; }
    [[nodiscard]] QComboBox* ssl_combo() const noexcept { return ssl_combo_; }
    [[nodiscard]] QLineEdit* root_cert_edit() const noexcept { return root_cert_edit_; }
    [[nodiscard]] QPushButton* browse_cert_button() const noexcept { return browse_cert_btn_; }
    [[nodiscard]] QSpinBox* timeout_spin() const noexcept { return timeout_spin_; }
    [[nodiscard]] QLineEdit* app_name_edit() const noexcept { return app_name_edit_; }
    [[nodiscard]] QCheckBox* disable_history_check() const noexcept { return disable_history_check_; }

signals:
    void paste_url_requested();
    void disable_history_toggled(bool checked);
    void auth_mode_changed();

private:
    void update_row_visibility();
    void on_browse_root_cert();
    void on_toggle_password_visibility();

    drivers::DriverUiDescriptor current_desc_     = drivers::driver_ui_descriptor(drivers::EngineKind::PostgreSql);
    bool                        controls_enabled_ = true;
    bool                        port_edited_      = false;
    bool                        db_edited_        = false;
    bool                        user_edited_      = false;
    bool                        password_visible_ = false;

    QTabWidget*  tabs_              = nullptr;
    QWidget*     general_tab_       = nullptr;
    QWidget*     security_tab_      = nullptr;
    QWidget*     advanced_tab_      = nullptr;
    QFormLayout* general_form_      = nullptr;
    QFormLayout* security_form_     = nullptr;
    QFormLayout* advanced_form_     = nullptr;

    QLineEdit*   host_edit_         = nullptr;
    QLineEdit*   port_edit_         = nullptr;
    QLineEdit*   database_edit_     = nullptr;
    QComboBox*   auth_combo_        = nullptr;
    QLineEdit*   username_edit_     = nullptr;
    QLineEdit*   password_edit_     = nullptr;
    QAction*     toggle_pwd_action_ = nullptr;
    QCheckBox*   remember_check_    = nullptr;
    QPushButton* paste_url_btn_     = nullptr;

    QComboBox*   ssl_combo_         = nullptr;
    QWidget*     cert_row_widget_   = nullptr;
    QLineEdit*   root_cert_edit_    = nullptr;
    QPushButton* browse_cert_btn_   = nullptr;

    QSpinBox*    timeout_spin_          = nullptr;
    QLineEdit*   app_name_edit_         = nullptr;
    QCheckBox*   disable_history_check_ = nullptr;
};

} // namespace capybaradb::ui
