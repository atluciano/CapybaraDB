#include "ui/editor/completion_popup.hpp"

#include "ui/shell/theme_tokens.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace capybaradb::ui {

CompletionPopupWidget::CompletionPopupWidget(QWidget* parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("IntelliSensePopup"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFrameShape(QFrame::Box);
    setStyleSheet(QStringLiteral(
        "QFrame#IntelliSensePopup { background-color: #FFFFFF; border: 1px solid #ABADB3; }"
        "QListWidget#IntelliSenseList { border: none; background-color: #FFFFFF; color: #1E1E1E; outline: none; }"
        "QListWidget#IntelliSenseList::item { padding: 2px 6px; }"
        "QListWidget#IntelliSenseList::item:selected { background-color: #0078D7; color: #FFFFFF; }"
        "QLabel#IntelliSenseDesc { background-color: #FFFFE1; color: #1E1E1E; border-top: 1px solid #C9C36B; padding: 3px 6px; }"));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("IntelliSenseList"));
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setUniformItemSizes(true);
    list_->setIconSize(QSize(16, 16));
    list_->setFont(theme::resolve_ui_font(12));
    lay->addWidget(list_, 1);

    desc_ = new QLabel(this);
    desc_->setObjectName(QStringLiteral("IntelliSenseDesc"));
    desc_->setFont(theme::resolve_ui_font(11));
    desc_->setWordWrap(true);
    desc_->setVisible(false);
    lay->addWidget(desc_);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
        update_description();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        explicit_choice_ = true;
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (const auto item = current_item()) {
            const QString txt = QString::fromStdString(item->text);
            const int start = replace_start_;
            close_popup();
            emit item_accepted(txt, start);
        }
    });
}

QIcon CompletionPopupWidget::icon_for_kind(core::SqlCompletionKind kind) {
    static std::array<QIcon, 7> cache{};
    const size_t key = (static_cast<size_t>(kind) < cache.size()) ? static_cast<size_t>(kind) : 0u;
    if (!cache[key].isNull()) {
        return cache[key];
    }

    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor col(0x1E, 0x6F, 0xC6);
    QString tag = QStringLiteral("K");
    switch (kind) {
    case core::SqlCompletionKind::Table:    col = QColor(0x2B, 0x57, 0x97); tag = QStringLiteral("T"); break;
    case core::SqlCompletionKind::View:     col = QColor(0x2D, 0x7D, 0x3B); tag = QStringLiteral("V"); break;
    case core::SqlCompletionKind::Column:   col = QColor(0x4B, 0x59, 0x64); tag = QStringLiteral("C"); break;
    case core::SqlCompletionKind::Function: col = QColor(0x7A, 0x3E, 0x9D); tag = QStringLiteral("f"); break;
    case core::SqlCompletionKind::Join:     col = QColor(0xC2, 0x5B, 0x12); tag = QStringLiteral("J"); break;
    case core::SqlCompletionKind::Schema:   col = QColor(0xB5, 0x83, 0x2A); tag = QStringLiteral("S"); break;
    case core::SqlCompletionKind::Keyword:
    default:                                col = QColor(0x1E, 0x6F, 0xC6); tag = QStringLiteral("K"); break;
    }

    p.setPen(QPen(col, 1.2));
    p.setBrush(QColor(col.red(), col.green(), col.blue(), 32));
    p.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 13.0), 2.5, 2.5);
    QFont f = theme::resolve_ui_font(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(col);
    p.drawText(QRectF(1, 1, 14, 14), Qt::AlignCenter, tag);

    cache[key] = QIcon(pm);
    return cache[key];
}

void CompletionPopupWidget::open_at(
    const QPoint& global_pos,
    int replace_start_char_pos,
    const QString& initial_prefix,
    std::vector<core::SqlCompletionItem> all_items) {
    all_items_       = std::move(all_items);
    replace_start_   = replace_start_char_pos;
    explicit_choice_ = false;

    const bool has_joins = std::any_of(all_items_.begin(), all_items_.end(), [](const auto& it) {
        return it.kind == core::SqlCompletionKind::Join;
    });
    const int width = has_joins ? 540 : 340;

    rebuild_list(initial_prefix);
    if (filtered_items_.empty()) {
        close_popup();
        return;
    }

    const int list_h = std::clamp(static_cast<int>(filtered_items_.size()) * 22 + 6, 48, 220);
    const int desc_h = desc_->isVisible() ? 38 : 0;
    resize(width, list_h + desc_h);

    QPoint pos = global_pos;
    if (QScreen* scr = QGuiApplication::screenAt(pos)) {
        const QRect avail = scr->availableGeometry();
        if (pos.x() + width > avail.right()) {
            pos.setX((std::max)(avail.left(), avail.right() - width));
        }
        if (pos.y() + height() > avail.bottom()) {
            pos.setY((std::max)(avail.top(), pos.y() - height() - 22));
        }
    }
    move(pos);
    show();
    raise();
}

void CompletionPopupWidget::update_filter(const QString& prefix) {
    if (!isVisible()) return;
    rebuild_list(prefix);
    if (filtered_items_.empty()) {
        close_popup();
    }
}

void CompletionPopupWidget::move_selection(int delta) {
    if (!isVisible() || list_->count() == 0) return;
    explicit_choice_ = true;
    const int cur = list_->currentRow();
    const int next = std::clamp((cur < 0 ? 0 : cur) + delta, 0, list_->count() - 1);
    list_->setCurrentRow(next);
}

void CompletionPopupWidget::close_popup() {
    hide();
    list_->clear();
    filtered_items_.clear();
    explicit_choice_ = false;
}

std::optional<core::SqlCompletionItem> CompletionPopupWidget::current_item() const {
    const int row = list_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= filtered_items_.size()) {
        return std::nullopt;
    }
    return filtered_items_[static_cast<size_t>(row)];
}

int CompletionPopupWidget::visible_count() const {
    return list_->count();
}

void CompletionPopupWidget::rebuild_list(const QString& prefix) {
    filtered_items_ = core::filter_sql_completion_items(all_items_, prefix.toStdString());
    // Cap rendered rows to 200 for instant typing response even on huge catalogs
    if (filtered_items_.size() > 200u) {
        filtered_items_.resize(200u);
    }

    list_->blockSignals(true);
    list_->clear();
    for (const core::SqlCompletionItem& it : filtered_items_) {
        auto* li = new QListWidgetItem(icon_for_kind(it.kind), QString::fromStdString(it.text), list_);
        list_->addItem(li);
    }
    if (!filtered_items_.empty()) {
        list_->setCurrentRow(0);
    }
    list_->blockSignals(false);
    update_description();
}

void CompletionPopupWidget::update_description() {
    const auto item = current_item();
    if (!item.has_value() || item->description.empty()) {
        desc_->setVisible(false);
        return;
    }
    desc_->setText(QString::fromStdString(item->description));
    desc_->setVisible(true);
}

} // namespace capybaradb::ui
