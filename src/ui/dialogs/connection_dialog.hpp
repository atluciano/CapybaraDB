#pragma once

#include "core/config/connection_profile.hpp"
#include "core/config/state_paths.hpp"
#include "core/threading/cancel_token.hpp"
#include "core/threading/catalog_worker_pool.hpp"
#include "drivers/common/db_types.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"
#include "ui/dialogs/connection_pages.hpp"
#include "ui/dialogs/profile_sidebar_widget.hpp"

#include <QDialog>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace capybaradb::ui {

class ConnectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(
        core::CatalogWorkerPool* worker_pool = nullptr,
        std::filesystem::path state_dir = {},
        QWidget* parent = nullptr);
    ~ConnectionDialog() override;

    [[nodiscard]] const drivers::ConnectionConfig& connection_config() const noexcept { return result_config_; }
    [[nodiscard]] QString display_name() const { return display_name_; }
    [[nodiscard]] QString selected_profile_id() const { return QString::fromStdString(selected_profile_id_); }
    [[nodiscard]] bool disable_query_history() const;

    void prefill(const drivers::ConnectionConfig& cfg);
    void select_profile_by_id(const QString& profile_id);
    void select_engine(drivers::EngineKind engine);
    [[nodiscard]] drivers::EngineKind current_engine() const noexcept { return current_engine_; }

    /// Parses and applies a connection URI or connection string (`postgresql://...`, `sqlite:///...`, `Server=...;`).
    bool apply_connection_uri(const QString& uri_text);

    // Master-detail component accessors
    [[nodiscard]] ProfileSidebarWidget* sidebar_widget() const noexcept { return sidebar_; }
    [[nodiscard]] QStackedWidget* pages_stack() const noexcept { return pages_stack_; }
    [[nodiscard]] ServerConnectionPage* server_page() const noexcept { return server_page_; }
    [[nodiscard]] FileConnectionPage* file_page() const noexcept { return file_page_; }
    [[nodiscard]] QToolButton* engine_button(drivers::EngineKind kind) const noexcept;

    // Accessors and hooks for testing and programmatic inspection
    [[nodiscard]] QComboBox* profiles_combo() const noexcept { return profiles_combo_; }
    [[nodiscard]] QLineEdit* profile_name_edit() const noexcept { return profile_name_edit_; }
    [[nodiscard]] QComboBox* profile_group_combo() const noexcept { return profile_group_combo_; }
    [[nodiscard]] QComboBox* engine_combo() const noexcept { return engine_combo_; }
    [[nodiscard]] QLineEdit* host_edit() const noexcept { return server_page_->host_edit(); }
    [[nodiscard]] QLineEdit* port_edit() const noexcept { return server_page_->port_edit(); }
    [[nodiscard]] QLineEdit* database_edit() const noexcept;
    [[nodiscard]] QComboBox* auth_combo() const noexcept { return server_page_->auth_combo(); }
    [[nodiscard]] QLineEdit* username_edit() const noexcept { return server_page_->username_edit(); }
    [[nodiscard]] QLineEdit* password_edit() const noexcept { return server_page_->password_edit(); }
    [[nodiscard]] QComboBox* ssl_combo() const noexcept { return server_page_->ssl_combo(); }
    [[nodiscard]] QLineEdit* root_cert_edit() const noexcept { return server_page_->root_cert_edit(); }
    [[nodiscard]] QCheckBox* remember_password_check() const noexcept { return server_page_->remember_password_check(); }
    [[nodiscard]] QCheckBox* disable_history_check() const noexcept;
    [[nodiscard]] QPushButton* save_profile_button() const noexcept { return save_profile_btn_; }
    [[nodiscard]] QPushButton* duplicate_profile_button() const noexcept { return sidebar_->duplicate_button(); }
    [[nodiscard]] QPushButton* delete_profile_button() const noexcept { return sidebar_->delete_button(); }
    [[nodiscard]] QPushButton* test_button() const noexcept { return test_btn_; }
    [[nodiscard]] QPushButton* cancel_test_button() const noexcept { return cancel_test_btn_; }
    [[nodiscard]] QPushButton* connect_button() const noexcept { return connect_btn_; }
    [[nodiscard]] QLabel* status_label() const noexcept { return status_label_; }
    [[nodiscard]] QLabel* subtitle_label() const noexcept { return subtitle_label_; }

    void set_delete_confirmation_handler(std::function<bool()> handler) { confirm_delete_fn_ = std::move(handler); }

public slots:
    void start_test_connection();
    void cancel_test_connection();

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    enum class StatusIconState : uint8_t {
        None = 0,
        Running,
        Success,
        Error,
    };

    void build_ui();
    void load_profiles(const std::string& select_id = {});
    void apply_profile(const core::ConnectionProfile& p);
    void on_profile_selected_by_id(const QString& profile_id);
    void on_profile_double_clicked(const QString& profile_id);
    void on_new_profile();
    void on_duplicate_profile();
    void on_save_profile();
    void on_delete_profile();
    void on_engine_selected(drivers::EngineKind engine);
    void on_paste_url();
    void on_accept_connect();

    bool try_build_config(drivers::ConnectionConfig& out);
    void set_testing_ui(bool testing);
    void set_status_text(
        const QString& text,
        const char* hex_color,
        StatusIconState icon_state = StatusIconState::None);
    void save_dialog_preferences(const std::string* last_profile_id = nullptr);
    void update_engine_specific_ui(bool apply_smart_defaults);

    std::filesystem::path                    state_dir_;
    core::CatalogWorkerPool*                 worker_pool_ = nullptr;
    std::unique_ptr<core::CatalogWorkerPool> owned_pool_;

    std::vector<core::ConnectionProfile> profiles_;
    std::string                          selected_profile_id_;
    drivers::EngineKind                  current_engine_ = drivers::EngineKind::PostgreSql;
    bool                                 syncing_form_   = false;

    std::shared_ptr<core::CancelToken>   test_cancel_;
    bool                                 testing_            = false;
    bool                                 test_canceled_user_ = false;
    bool                                 close_after_test_   = false;
    uint64_t                             test_generation_    = 0;

    drivers::ConnectionConfig result_config_;
    QString                   display_name_;
    std::function<bool()>     confirm_delete_fn_;

    // Banner
    QLabel* subtitle_label_ = nullptr;

    // Master-Detail Components
    ProfileSidebarWidget*        sidebar_              = nullptr;
    QButtonGroup*                engine_btn_group_     = nullptr;
    std::array<QToolButton*, 3>  engine_buttons_{};
    QLineEdit*                   profile_name_edit_    = nullptr;
    QComboBox*                   profile_group_combo_  = nullptr;
    QStackedWidget*              pages_stack_          = nullptr;
    ServerConnectionPage*        server_page_          = nullptr;
    FileConnectionPage*          file_page_            = nullptr;

    // Synchronized hidden combo proxies for programmatic inspection
    QComboBox* profiles_combo_ = nullptr;
    QComboBox* engine_combo_   = nullptr;

    // Status & Footer buttons
    QLabel*      status_icon_label_ = nullptr;
    QLabel*      status_label_      = nullptr;
    QPushButton* cancel_test_btn_   = nullptr;
    QPushButton* test_btn_          = nullptr;
    QPushButton* save_profile_btn_  = nullptr;
    QPushButton* connect_btn_       = nullptr;
    QPushButton* cancel_btn_        = nullptr;
};

} // namespace capybaradb::ui
