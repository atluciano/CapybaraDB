#include "ui/dialogs/filter_settings_dialog.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace capybaradb::ui {
namespace {

QComboBox* make_operator_combo(drivers::CatalogFilterOperator op, QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItem(QStringLiteral("Contains"), static_cast<int>(drivers::CatalogFilterOperator::Contains));
    combo->addItem(QStringLiteral("Equals"), static_cast<int>(drivers::CatalogFilterOperator::Equals));
    combo->addItem(QStringLiteral("Does not contain"), static_cast<int>(drivers::CatalogFilterOperator::DoesNotContain));
    combo->setFrame(false);
    const int idx = combo->findData(static_cast<int>(op));
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
    return combo;
}

} // namespace

FilterSettingsDialog::FilterSettingsDialog(
    const QString& server,
    const QString& database,
    const drivers::CatalogFilter& current,
    bool can_filter_by_owner,
    QWidget* parent)
    : QDialog(parent)
    , result_(current) {
    setObjectName(QStringLiteral("FilterSettingsDialog"));
    setWindowTitle(QStringLiteral("Filter Settings"));
    setMinimumSize(420, 320);
    resize(540, 440);
    build_ui(server, database, current, can_filter_by_owner);
}

void FilterSettingsDialog::build_ui(
    const QString& server,
    const QString& database,
    const drivers::CatalogFilter& current,
    bool can_filter_by_owner) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);

    auto* header_grid = new QGridLayout();
    header_grid->setContentsMargins(0, 0, 0, 0);
    header_grid->setHorizontalSpacing(8);
    header_grid->setVerticalSpacing(6);
    header_grid->setColumnMinimumWidth(0, 120);
    header_grid->setColumnStretch(1, 1);

    header_grid->addWidget(new QLabel(QStringLiteral("Server:"), this), 0, 0);
    header_grid->addWidget(new QLabel(server, this), 0, 1);

    if (!database.isEmpty()) {
        header_grid->addWidget(new QLabel(QStringLiteral("Database:"), this), 1, 0);
        header_grid->addWidget(new QLabel(database, this), 1, 1);
    }
    root->addLayout(header_grid);

    root->addWidget(new QLabel(QStringLiteral("Filter Criteria:"), this));

    table_ = new QTableWidget(can_filter_by_owner ? 2 : 1, 3, this);
    table_->setObjectName(QStringLiteral("FilterCriteriaTable"));
    table_->setAccessibleName(QStringLiteral("Filter criteria"));
    table_->setHorizontalHeaderLabels({
        QStringLiteral("Property"),
        QStringLiteral("Operator"),
        QStringLiteral("Value"),
    });
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(23);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Row 0: Name
    descriptions_.push_back(QStringLiteral("Include or exclude objects based on the name or part of a name."));
    auto* name_prop = new QTableWidgetItem(QStringLiteral("Name"));
    name_prop->setFlags(name_prop->flags() & ~Qt::ItemIsEditable);
    table_->setItem(0, 0, name_prop);

    name_op_ = make_operator_combo(current.name_operator, table_);
    name_op_->setAccessibleName(QStringLiteral("Name operator"));
    table_->setCellWidget(0, 1, name_op_);

    name_edit_ = new QLineEdit(QString::fromStdString(current.name), table_);
    name_edit_->setAccessibleName(QStringLiteral("Name value"));
    name_edit_->setFrame(false);
    table_->setCellWidget(0, 2, name_edit_);

    if (can_filter_by_owner) {
        descriptions_.push_back(QStringLiteral("Include or exclude objects based on the role that owns them."));
        auto* owner_prop = new QTableWidgetItem(QStringLiteral("Owner"));
        owner_prop->setFlags(owner_prop->flags() & ~Qt::ItemIsEditable);
        table_->setItem(1, 0, owner_prop);

        owner_op_ = make_operator_combo(current.owner_operator, table_);
        owner_op_->setAccessibleName(QStringLiteral("Owner operator"));
        table_->setCellWidget(1, 1, owner_op_);

        owner_edit_ = new QLineEdit(QString::fromStdString(current.owner), table_);
        owner_edit_->setAccessibleName(QStringLiteral("Owner value"));
        owner_edit_->setFrame(false);
        table_->setCellWidget(1, 2, owner_edit_);
    }

    root->addWidget(table_, 1);

    auto* desc_frame = new QFrame(this);
    desc_frame->setObjectName(QStringLiteral("FilterDescriptionBox"));
    desc_frame->setFixedHeight(56);
    auto* desc_layout = new QVBoxLayout(desc_frame);
    desc_layout->setContentsMargins(6, 4, 6, 4);
    desc_label_ = new QLabel(descriptions_[0], desc_frame);
    desc_label_->setWordWrap(true);
    desc_layout->addWidget(desc_label_);
    root->addWidget(desc_frame);

    table_->selectRow(0);
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int cur_row, int, int, int) {
        if (cur_row >= 0 && cur_row < static_cast<int>(descriptions_.size())) {
            desc_label_->setText(descriptions_[static_cast<size_t>(cur_row)]);
        }
    });

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(8);
    footer->addStretch(1);

    clear_btn_ = new QPushButton(QStringLiteral("&Clear Filter"), this);
    clear_btn_->setAutoDefault(false);
    footer->addWidget(clear_btn_);

    ok_btn_ = new QPushButton(QStringLiteral("OK"), this);
    ok_btn_->setMinimumWidth(80);
    ok_btn_->setDefault(true);
    footer->addWidget(ok_btn_);

    auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_btn->setAutoDefault(false);
    footer->addWidget(cancel_btn);

    root->addLayout(footer);

    connect(clear_btn_, &QPushButton::clicked, this, &FilterSettingsDialog::on_clear_filter);
    connect(ok_btn_, &QPushButton::clicked, this, &FilterSettingsDialog::on_accept);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
}

void FilterSettingsDialog::on_clear_filter() {
    name_edit_->clear();
    if (owner_edit_ != nullptr) {
        owner_edit_->clear();
    }
}

void FilterSettingsDialog::on_accept() {
    result_.name = name_edit_->text().trimmed().toStdString();
    result_.name_operator = static_cast<drivers::CatalogFilterOperator>(name_op_->currentData().toInt());
    if (owner_edit_ != nullptr && owner_op_ != nullptr) {
        result_.owner = owner_edit_->text().trimmed().toStdString();
        result_.owner_operator = static_cast<drivers::CatalogFilterOperator>(owner_op_->currentData().toInt());
    } else {
        result_.owner.clear();
        result_.owner_operator = drivers::CatalogFilterOperator::Equals;
    }
    accept();
}

} // namespace capybaradb::ui
