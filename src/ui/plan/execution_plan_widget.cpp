#include "ui/plan/execution_plan_widget.hpp"

#include "ui/shell/theme_tokens.hpp"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace capybaradb::ui {
namespace detail {

namespace {

constexpr double kMinZoom    = 0.1;
constexpr double kMaxZoom    = 4.0;
constexpr double kZoomStep   = 1.25;
constexpr double kColumnGap  = 62.0;
constexpr double kRowGap     = 26.0;
constexpr double kIconSize   = 26.0;
constexpr double kMargin     = 14.0;

double arrow_thickness(double rows) noexcept {
    return std::clamp(2.0 + std::log10((std::max)(rows, 0.0) + 1.0) * 1.5, 2.0, 12.0);
}

QString html_escape(const QString& s) {
    QString out;
    out.reserve(s.size() + 16);
    for (QChar ch : s) {
        switch (ch.unicode()) {
        case '&':  out += QStringLiteral("&amp;"); break;
        case '<':  out += QStringLiteral("&lt;"); break;
        case '>':  out += QStringLiteral("&gt;"); break;
        case '"':  out += QStringLiteral("&quot;"); break;
        default:   out.append(ch); break;
        }
    }
    return out;
}

QString build_operator_tooltip_html(const core::ExecutionPlanOperator& op) {
    QString html = QStringLiteral(
        "<div style='background:#FFFFE1; color:#1E1E1E; padding:4px; min-width:260px; max-width:420px;'>"
        "<div style='font-weight:bold; text-align:center; margin-bottom:3px;'>%1</div>")
        .arg(html_escape(QString::fromStdString(op.title)));
    if (!op.description.empty()) {
        html += QStringLiteral("<div style='margin-bottom:6px;'>%1</div>")
            .arg(html_escape(QString::fromStdString(op.description)));
    }
    if (!op.summary.empty()) {
        html += QStringLiteral("<table cellspacing='0' cellpadding='2' width='100%' style='border-collapse:collapse;'>");
        for (const auto& [k, v] : op.summary) {
            html += QStringLiteral(
                "<tr><td style='border-bottom:1px solid #C9C36B; font-weight:bold;'>%1</td>"
                "<td align='right' style='border-bottom:1px solid #C9C36B; padding-left:10px;'>%2</td></tr>")
                .arg(html_escape(QString::fromStdString(k)), html_escape(QString::fromStdString(v)));
        }
        html += QStringLiteral("</table>");
    }
    for (const auto& [label, text] : op.details) {
        QString t = QString::fromStdString(text);
        if (t.size() > 500) t = t.left(500) + QStringLiteral("…");
        html += QStringLiteral("<div style='font-weight:bold; margin-top:6px;'>%1</div><div>%2</div>")
            .arg(html_escape(QString::fromStdString(label)), html_escape(t));
    }
    if (!op.warnings.empty()) {
        html += QStringLiteral("<div style='font-weight:bold; color:#8A6D00; margin-top:6px;'>Warnings</div>");
        for (const std::string& w : op.warnings) {
            html += QStringLiteral("<div>%1</div>").arg(html_escape(QString::fromStdString(w)));
        }
    }
    html += QStringLiteral("</div>");
    return html;
}

QString build_arrow_tooltip_html(const core::ExecutionPlanOperator& child) {
    QString html = QStringLiteral(
        "<div style='background:#FFFFE1; color:#1E1E1E; padding:4px; min-width:220px;'>"
        "<table cellspacing='0' cellpadding='2' width='100%' style='border-collapse:collapse;'>");
    if (child.actual_rows.has_value()) {
        html += QStringLiteral(
            "<tr><td style='border-bottom:1px solid #C9C36B; font-weight:bold;'>Actual Number of Rows</td>"
            "<td align='right' style='border-bottom:1px solid #C9C36B; padding-left:10px;'>%1</td></tr>")
            .arg(QString::number(*child.actual_rows, 'f', 0));
    }
    html += QStringLiteral(
        "<tr><td style='border-bottom:1px solid #C9C36B; font-weight:bold;'>Estimated Number of Rows</td>"
        "<td align='right' style='border-bottom:1px solid #C9C36B; padding-left:10px;'>%1</td></tr>")
        .arg(QString::number(child.estimated_rows, 'f', 1));
    if (const auto pw = child.property_value("Plan Width")) {
        html += QStringLiteral(
            "<tr><td style='border-bottom:1px solid #C9C36B; font-weight:bold;'>Estimated Row Size</td>"
            "<td align='right' style='border-bottom:1px solid #C9C36B; padding-left:10px;'>%1 B</td></tr>")
            .arg(html_escape(QString::fromStdString(std::string(*pw))));
    }
    html += QStringLiteral("</table></div>");
    return html;
}

void draw_operator_icon(QPainter& p, const QRectF& r, core::ExecutionPlanIcon icon, bool has_warnings) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor stroke = theme::color(theme::kPlanIconStroke);
    const QColor fill   = theme::color(theme::kPlanIconFill);
    p.setPen(QPen(stroke, 1.3));
    p.setBrush(fill);

    const double x = r.x();
    const double y = r.y();
    const double w = r.width();
    const double h = r.height();

    switch (icon) {
    case core::ExecutionPlanIcon::Statement:
        p.drawRect(QRectF(x + 2, y + 3, w - 4, h - 6));
        p.drawLine(QPointF(x + 2, y + 8), QPointF(x + w - 2, y + 8));
        p.drawLine(QPointF(x + 2, y + 14), QPointF(x + w - 2, y + 14));
        p.drawLine(QPointF(x + 9, y + 8), QPointF(x + 9, y + h - 3));
        p.drawLine(QPointF(x + 16, y + 8), QPointF(x + 16, y + h - 3));
        break;
    case core::ExecutionPlanIcon::TableScan:
        p.drawRect(QRectF(x + 2, y + 4, w - 9, h - 8));
        p.drawLine(QPointF(x + 2, y + 10), QPointF(x + w - 7, y + 10));
        p.drawLine(QPointF(x + 2, y + 16), QPointF(x + w - 7, y + 16));
        p.drawPolygon(QPolygonF({QPointF(x + w - 6, y + 8), QPointF(x + w - 1, y + 13), QPointF(x + w - 6, y + 18)}));
        break;
    case core::ExecutionPlanIcon::IndexScan:
        p.drawPolygon(QPolygonF({QPointF(x + w / 2, y + 2), QPointF(x + w / 2 + 6, y + 8), QPointF(x + w / 2, y + 14), QPointF(x + w / 2 - 6, y + 8)}));
        p.drawLine(QPointF(x + w / 2, y + 14), QPointF(x + w / 2, y + 19));
        p.drawLine(QPointF(x + 4, y + 19), QPointF(x + w - 4, y + 19));
        p.drawRect(QRectF(x + 1, y + 20, 6, 5));
        p.drawRect(QRectF(x + 10, y + 20, 6, 5));
        p.drawRect(QRectF(x + 19, y + 20, 6, 5));
        break;
    case core::ExecutionPlanIcon::BitmapScan:
        p.drawRect(QRectF(x + 2, y + 2, 9, 9));
        p.drawRect(QRectF(x + 14, y + 2, 10, 9));
        p.drawRect(QRectF(x + 2, y + 14, 9, 10));
        p.drawRect(QRectF(x + 14, y + 14, 10, 10));
        break;
    case core::ExecutionPlanIcon::Join:
        p.drawLine(QPointF(x + w - 2, y + 6), QPointF(x + 13, y + 6));
        p.drawLine(QPointF(x + 13, y + 6), QPointF(x + 7, y + h / 2));
        p.drawLine(QPointF(x + w - 2, y + h - 6), QPointF(x + 13, y + h - 6));
        p.drawLine(QPointF(x + 13, y + h - 6), QPointF(x + 7, y + h / 2));
        p.drawPolygon(QPolygonF({QPointF(x + 2, y + h / 2), QPointF(x + 8, y + h / 2 - 4), QPointF(x + 8, y + h / 2 + 4)}));
        break;
    case core::ExecutionPlanIcon::Sort:
        p.drawRect(QRectF(x + 2, y + 3, 8, 5));
        p.drawRect(QRectF(x + 2, y + 11, 12, 5));
        p.drawRect(QRectF(x + 2, y + 19, 16, 5));
        p.drawLine(QPointF(x + 21, y + 3), QPointF(x + 21, y + 22));
        p.drawLine(QPointF(x + 18, y + 18), QPointF(x + 21, y + 23));
        p.drawLine(QPointF(x + 24, y + 18), QPointF(x + 21, y + 23));
        break;
    case core::ExecutionPlanIcon::Aggregate: {
        QPainterPath sigma;
        sigma.moveTo(x + w - 4, y + 4);
        sigma.lineTo(x + 5, y + 4);
        sigma.lineTo(x + 14, y + h / 2);
        sigma.lineTo(x + 5, y + h - 4);
        sigma.lineTo(x + w - 4, y + h - 4);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(stroke, 2.0));
        p.drawPath(sigma);
        break;
    }
    case core::ExecutionPlanIcon::Limit:
        p.drawRect(QRectF(x + 2, y + 5, 14, 16));
        p.drawLine(QPointF(x + 2, y + 11), QPointF(x + 16, y + 11));
        p.drawLine(QPointF(x + 2, y + 16), QPointF(x + 16, y + 16));
        p.setPen(QPen(stroke, 2.0));
        p.drawLine(QPointF(x + 21, y + 3), QPointF(x + 21, y + 23));
        break;
    case core::ExecutionPlanIcon::Materialize:
        p.drawRoundedRect(QRectF(x + 3, y + 4, w - 6, h - 8), 4, 4);
        p.drawEllipse(QRectF(x + 3, y + 3, w - 6, 6));
        break;
    case core::ExecutionPlanIcon::Append:
        p.drawRect(QRectF(x + 2, y + 3, w - 4, 7));
        p.drawRect(QRectF(x + 2, y + 15, w - 4, 7));
        p.drawLine(QPointF(x + w / 2, y + 10), QPointF(x + w / 2, y + 15));
        break;
    case core::ExecutionPlanIcon::Gather:
        p.drawLine(QPointF(x + 3, y + 5), QPointF(x + 10, y + h / 2));
        p.drawLine(QPointF(x + 10, y + h / 2), QPointF(x + 3, y + h - 5));
        p.drawLine(QPointF(x + w - 3, y + 5), QPointF(x + w - 10, y + h / 2));
        p.drawLine(QPointF(x + w - 10, y + h / 2), QPointF(x + w - 3, y + h - 5));
        p.drawLine(QPointF(x + 7, y + h / 2), QPointF(x + w - 7, y + h / 2));
        break;
    case core::ExecutionPlanIcon::Modify:
        p.drawPolygon(QPolygonF({QPointF(x + 3, y + 23), QPointF(x + 3, y + 17), QPointF(x + 17, y + 3), QPointF(x + 23, y + 9), QPointF(x + 9, y + 23)}));
        break;
    case core::ExecutionPlanIcon::Subquery:
        p.drawRect(QRectF(x + 2, y + 2, w - 4, h - 4));
        p.drawRect(QRectF(x + 7, y + 8, w - 14, h - 16));
        break;
    default:
        p.drawRect(QRectF(x + 3, y + 3, w - 6, h - 6));
        p.drawLine(QPointF(x + w / 2, y + 8), QPointF(x + w / 2, y + h - 8));
        p.drawLine(QPointF(x + 8, y + h / 2), QPointF(x + w - 8, y + h / 2));
        break;
    }

    if (has_warnings) {
        // Yellow warning triangle over lower-right corner
        const QPointF p0(x + w - 5, y + h - 11);
        const QPointF p1(x + w + 2, y + h + 1);
        const QPointF p2(x + w - 12, y + h + 1);
        p.setBrush(QColor(0xFF, 0xE1, 0x1A));
        p.setPen(QPen(QColor(0x8A, 0x6D, 0x00), 1.0));
        p.drawPolygon(QPolygonF({p0, p1, p2}));
        p.setPen(QPen(Qt::black, 1.4));
        p.drawLine(QPointF(x + w - 5, y + h - 7.5), QPointF(x + w - 5, y + h - 3.5));
        p.drawPoint(QPointF(x + w - 5, y + h - 1.5));
    }
    p.restore();
}

} // namespace

class PlanDiagramCanvas final : public QWidget {
public:
    struct PlacedOp {
        const core::ExecutionPlanOperator*  op   = nullptr;
        const core::ExecutionPlanStatement* stmt = nullptr;
        QRectF                              bounds;      // Total node rect in logical coordinates
        QRectF                              icon_rect;
        QRectF                              label_rect;
        QStringList                         lines;
    };

    struct PlacedArrow {
        const core::ExecutionPlanOperator* child = nullptr;
        QPolygonF                          polyline;
        double                             thickness = 2.0;
        QPolygonF                          arrowhead;
    };

    struct PlacedSection {
        const core::ExecutionPlanStatement* stmt = nullptr;
        QRectF                              header_rect;
        QString                             header_line;
        QString                             sql_line;
        QString                             missing_idx_line;
        double                              diagram_top = 0.0;
    };

    explicit PlanDiagramCanvas(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMouseTracking(true);
        setBackgroundRole(QPalette::Base);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Base, Qt::white);
        setPalette(pal);
        diagram_font_ = QFont(QStringLiteral("Courier New"), 9);
        diagram_font_.setPixelSize(11);
    }

    std::function<void(const core::ExecutionPlanOperator*, const core::ExecutionPlanStatement*)> on_select;
    std::function<void(const QPoint& global_pos)> on_context_menu;
    std::function<void(double)> on_zoom_changed;

    void set_statements(const std::vector<core::ExecutionPlanStatement>* stmts) {
        stmts_ = stmts;
        selected_ = nullptr;
        rebuild_layout();
    }

    void set_selected(const core::ExecutionPlanOperator* op) {
        selected_ = op;
        update();
    }

    [[nodiscard]] double zoom() const noexcept { return zoom_; }

    void set_zoom(double z) {
        zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
        update_scaled_size();
        update();
        if (on_zoom_changed) on_zoom_changed(zoom_);
    }

    [[nodiscard]] QSizeF logical_size() const noexcept { return logical_size_; }

    [[nodiscard]] QRectF rect_for_operator(const core::ExecutionPlanOperator* op) const {
        for (const PlacedOp& po : placed_ops_) {
            if (po.op == op) {
                return QRectF(
                    po.bounds.x() * zoom_,
                    po.bounds.y() * zoom_,
                    po.bounds.width() * zoom_,
                    po.bounds.height() * zoom_);
            }
        }
        return {};
    }

    [[nodiscard]] int operator_count() const noexcept { return static_cast<int>(placed_ops_.size()); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        if (stmts_ == nullptr || stmts_->empty()) {
            return;
        }
        p.scale(zoom_, zoom_);
        p.setFont(diagram_font_);
        const QFontMetricsF fm(diagram_font_);

        // 1. Statement headers
        for (const PlacedSection& sec : sections_) {
            p.setPen(QPen(theme::color(theme::kGridOuterBorder), 1.0));
            p.setBrush(QColor(0xFA, 0xFC, 0xFE));
            p.drawRect(sec.header_rect);

            double y = sec.header_rect.top() + 5.0;
            const double x = sec.header_rect.left() + 8.0;
            const double w = sec.header_rect.width() - 16.0;
            p.setPen(theme::color(theme::kPrimaryText));
            p.drawText(QRectF(x, y, w, fm.height() + 2), Qt::AlignLeft | Qt::AlignVCenter, sec.header_line);
            y += fm.height() + 3.0;
            if (!sec.sql_line.isEmpty()) {
                const QString elided = fm.elidedText(sec.sql_line, Qt::ElideRight, w);
                p.drawText(QRectF(x, y, w, fm.height() + 2), Qt::AlignLeft | Qt::AlignVCenter, elided);
                y += fm.height() + 3.0;
            }
            if (!sec.missing_idx_line.isEmpty()) {
                p.setPen(QColor(0x00, 0x80, 0x00));
                const QString elided = fm.elidedText(sec.missing_idx_line, Qt::ElideRight, w);
                p.drawText(QRectF(x, y, w, fm.height() + 2), Qt::AlignLeft | Qt::AlignVCenter, elided);
            }
        }

        // 2. Arrows
        const QColor arrow_col = theme::color(theme::kPlanArrow);
        for (const PlacedArrow& arr : arrows_) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(arrow_col, arr.thickness, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(arr.polyline);
            if (arr.thickness > 2.5) {
                p.setPen(QPen(Qt::white, arr.thickness - 2.0, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
                p.drawPolyline(arr.polyline);
            }
            p.setPen(QPen(arrow_col, 1.0));
            p.setBrush(Qt::white);
            p.drawPolygon(arr.arrowhead);
            p.restore();
        }

        // 3. Operator boxes
        const QColor sel_bg = theme::color(theme::kPlanSelection);
        for (const PlacedOp& po : placed_ops_) {
            draw_operator_icon(p, po.icon_rect, po.op->icon, !po.op->warnings.empty());
            const bool is_sel = (po.op == selected_);
            if (is_sel) {
                p.fillRect(po.label_rect, sel_bg);
                p.setPen(Qt::white);
            } else {
                p.setPen(Qt::black);
            }
            double ly = po.label_rect.top() + 2.0;
            for (const QString& ln : po.lines) {
                p.drawText(
                    QRectF(po.label_rect.left() + 4.0, ly, po.label_rect.width() - 8.0, fm.height()),
                    Qt::AlignHCenter | Qt::AlignVCenter,
                    ln);
                ly += fm.height() + 1.0;
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            const QPointF pt = event->position() / zoom_;
            for (const PlacedOp& po : placed_ops_) {
                if (po.bounds.contains(pt)) {
                    if (on_select) on_select(po.op, po.stmt);
                    event->accept();
                    return;
                }
            }
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QPointF pt = event->position() / zoom_;
        for (const PlacedOp& po : placed_ops_) {
            if (po.bounds.contains(pt)) {
                QToolTip::showText(event->globalPosition().toPoint(), build_operator_tooltip_html(*po.op), this);
                return;
            }
        }
        for (const PlacedArrow& arr : arrows_) {
            if (is_near_polyline(pt, arr.polyline, (std::max)(arr.thickness, 10.0) * 0.5)) {
                QToolTip::showText(event->globalPosition().toPoint(), build_arrow_tooltip_html(*arr.child), this);
                return;
            }
        }
        QToolTip::hideText();
        QWidget::mouseMoveEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            set_zoom(event->angleDelta().y() > 0 ? zoom_ * kZoomStep : zoom_ / kZoomStep);
            event->accept();
            return;
        }
        QWidget::wheelEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        if (on_context_menu) {
            on_context_menu(event->globalPos());
            event->accept();
            return;
        }
        QWidget::contextMenuEvent(event);
    }

private:
    static bool is_near_polyline(const QPointF& p, const QPolygonF& poly, double tol) {
        const double tol2 = tol * tol;
        for (int i = 0; i + 1 < poly.size(); ++i) {
            const QPointF a = poly[i];
            const QPointF b = poly[i + 1];
            const double dx = b.x() - a.x();
            const double dy = b.y() - a.y();
            const double len2 = dx * dx + dy * dy;
            double t = 0.0;
            if (len2 > 1e-6) {
                t = std::clamp(((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2, 0.0, 1.0);
            }
            const double px = a.x() + t * dx - p.x();
            const double py = a.y() + t * dy - p.y();
            if (px * px + py * py <= tol2) return true;
        }
        return false;
    }

    void rebuild_layout() {
        sections_.clear();
        placed_ops_.clear();
        arrows_.clear();
        logical_size_ = QSizeF(400.0, 200.0);

        if (stmts_ == nullptr || stmts_->empty()) {
            update_scaled_size();
            update();
            return;
        }

        const QFontMetricsF fm(diagram_font_);
        const double line_h = fm.height() + 1.0;

        auto measure_op = [&](const core::ExecutionPlanOperator& op, QStringList& lines_out) -> QSizeF {
            lines_out.clear();
            double max_w = kIconSize + 12.0;
            for (const std::string& s : op.display_lines()) {
                const QString qs = QString::fromStdString(s);
                lines_out.push_back(qs);
                max_w = (std::max)(max_w, fm.horizontalAdvance(qs) + 12.0);
            }
            const double label_h = static_cast<double>(lines_out.size()) * line_h + 4.0;
            return QSizeF(max_w, kIconSize + 4.0 + label_h);
        };

        double cursor_y = kMargin;
        double max_right = 560.0;

        for (const core::ExecutionPlanStatement& stmt : *stmts_) {
            PlacedSection sec;
            sec.stmt        = &stmt;
            sec.header_line = QString::fromStdString(stmt.header());
            sec.sql_line    = QString::fromStdString(stmt.query_text);
            if (!stmt.missing_indexes.empty()) {
                QStringList mi;
                for (const auto& s : stmt.missing_indexes) {
                    mi.push_back(QString::fromStdString(s.header_line));
                }
                sec.missing_idx_line = mi.join(QStringLiteral(" | "));
            }
            const int header_rows = 1 + (sec.sql_line.isEmpty() ? 0 : 1) + (sec.missing_idx_line.isEmpty() ? 0 : 1);
            const double header_h = 10.0 + static_cast<double>(header_rows) * (fm.height() + 3.0);

            // Layout operator tree (ExecutionPlanLayout algorithm: root left, inputs right)
            std::unordered_map<const core::ExecutionPlanOperator*, QSizeF> sizes;
            std::unordered_map<const core::ExecutionPlanOperator*, QStringList> lines_map;
            std::vector<double> col_widths;

            std::function<void(const core::ExecutionPlanOperator&, size_t)> measure_tree =
                [&](const core::ExecutionPlanOperator& op, size_t depth) {
                    QStringList ls;
                    const QSizeF sz = measure_op(op, ls);
                    sizes[&op]     = sz;
                    lines_map[&op] = std::move(ls);
                    if (col_widths.size() <= depth) col_widths.resize(depth + 1, 0.0);
                    col_widths[depth] = (std::max)(col_widths[depth], sz.width());
                    for (const auto& ch : op.children) measure_tree(ch, depth + 1);
                };
            measure_tree(stmt.root, 0);

            std::vector<double> col_lefts(col_widths.size(), kMargin);
            for (size_t d = 1; d < col_widths.size(); ++d) {
                col_lefts[d] = col_lefts[d - 1] + col_widths[d - 1] + kColumnGap;
            }

            std::unordered_map<const core::ExecutionPlanOperator*, double> centers;
            std::function<void(const core::ExecutionPlanOperator&, double)> shift_tree =
                [&](const core::ExecutionPlanOperator& op, double delta) {
                    centers[&op] += delta;
                    for (const auto& ch : op.children) shift_tree(ch, delta);
                };

            std::function<double(const core::ExecutionPlanOperator&, double)> lay_tree =
                [&](const core::ExecutionPlanOperator& op, double top) -> double {
                    const double half = sizes[&op].height() * 0.5;
                    if (op.children.empty()) {
                        centers[&op] = top + half;
                        return top + half * 2.0;
                    }
                    double bottom = top;
                    for (size_t i = 0; i < op.children.size(); ++i) {
                        bottom = lay_tree(op.children[i], i == 0 ? top : bottom + kRowGap);
                    }
                    centers[&op] = centers[&op.children[0]];
                    const double overflow = top + half - centers[&op];
                    if (overflow > 0.0) {
                        shift_tree(op, overflow);
                        bottom += overflow;
                    }
                    return (std::max)(bottom, centers[&op] + half);
                };

            const double diagram_top = cursor_y + header_h + 12.0;
            const double diagram_bottom = lay_tree(stmt.root, diagram_top);

            std::unordered_map<const core::ExecutionPlanOperator*, QRectF> rects;
            double section_right = 560.0;
            std::function<void(const core::ExecutionPlanOperator&, size_t)> collect_rects =
                [&](const core::ExecutionPlanOperator& op, size_t depth) {
                    const QSizeF sz = sizes[&op];
                    const double x = col_lefts[depth] + (col_widths[depth] - sz.width()) * 0.5;
                    const double y = centers[&op] - sz.height() * 0.5;
                    const QRectF r(x, y, sz.width(), sz.height());
                    rects[&op] = r;
                    section_right = (std::max)(section_right, r.right() + kMargin);

                    PlacedOp po;
                    po.op         = &op;
                    po.stmt       = &stmt;
                    po.bounds     = r;
                    po.icon_rect  = QRectF(r.center().x() - kIconSize * 0.5, r.top(), kIconSize, kIconSize);
                    po.label_rect = QRectF(r.left(), r.top() + kIconSize + 4.0, r.width(), r.height() - kIconSize - 4.0);
                    po.lines      = lines_map[&op];
                    placed_ops_.push_back(std::move(po));

                    for (const auto& ch : op.children) collect_rects(ch, depth + 1);
                };
            collect_rects(stmt.root, 0);

            // Arrows between parent and children
            std::function<void(const core::ExecutionPlanOperator&)> collect_arrows =
                [&](const core::ExecutionPlanOperator& parent) {
                    const QRectF pr = rects[&parent];
                    const size_t cnt = parent.children.size();
                    for (size_t i = 0; i < cnt; ++i) {
                        const core::ExecutionPlanOperator& ch = parent.children[i];
                        const QRectF sr = rects[&ch];
                        const double thick = arrow_thickness(ch.flow_rows());
                        const double head = 7.0;
                        const double end_x = pr.right() + 3.0;
                        const double end_y = pr.top() + kIconSize * 0.5 + static_cast<double>(i) * 8.0;
                        const double start_x = sr.left() - 3.0;
                        const double start_y = sr.top() + kIconSize * 0.5;
                        const double shaft_x = end_x + head;

                        QPolygonF poly;
                        poly << QPointF(start_x, start_y);
                        if (std::abs(start_y - end_y) >= 1.0) {
                            const double bus = (std::min)(start_x - 8.0, shaft_x + 22.0 + static_cast<double>(cnt - 1 - i) * 8.0);
                            poly << QPointF(bus, start_y) << QPointF(bus, end_y);
                        }
                        poly << QPointF(shaft_x, end_y);

                        const double wing = (std::max)(4.0, thick * 0.5 + 3.0);
                        QPolygonF head_poly;
                        head_poly << QPointF(end_x, end_y) << QPointF(shaft_x, end_y - wing) << QPointF(shaft_x, end_y + wing);

                        arrows_.push_back(PlacedArrow{&ch, std::move(poly), thick, std::move(head_poly)});
                        collect_arrows(ch);
                    }
                };
            collect_arrows(stmt.root);

            max_right = (std::max)(max_right, section_right);
            sec.header_rect = QRectF(kMargin, cursor_y, section_right - kMargin * 2.0, header_h);
            sec.diagram_top = diagram_top;
            sections_.push_back(std::move(sec));

            cursor_y = diagram_bottom + 24.0;
        }

        // Normalize all header widths to full canvas width
        for (PlacedSection& sec : sections_) {
            sec.header_rect.setWidth((std::max)(sec.header_rect.width(), max_right - kMargin * 2.0));
        }
        logical_size_ = QSizeF(max_right + kMargin, cursor_y + kMargin);
        update_scaled_size();
        update();
    }

    void update_scaled_size() {
        const int w = (std::max)(200, static_cast<int>(std::ceil(logical_size_.width() * zoom_)));
        const int h = (std::max)(120, static_cast<int>(std::ceil(logical_size_.height() * zoom_)));
        setFixedSize(w, h);
    }

    const std::vector<core::ExecutionPlanStatement>* stmts_    = nullptr;
    const core::ExecutionPlanOperator*               selected_ = nullptr;
    QFont                                            diagram_font_;
    double                                           zoom_     = 1.0;
    QSizeF                                           logical_size_{400.0, 200.0};
    std::vector<PlacedSection>                       sections_;
    std::vector<PlacedOp>                            placed_ops_;
    std::vector<PlacedArrow>                         arrows_;
};

} // namespace detail

ExecutionPlanWidget::ExecutionPlanWidget(bool allow_open_in_new_tab, QWidget* parent)
    : QWidget(parent)
    , allow_open_in_new_tab_(allow_open_in_new_tab) {
    setObjectName(QStringLiteral("ExecutionPlanWidget"));
    build_ui();
    update_action_states();
}

void ExecutionPlanWidget::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Plan Toolbar (#D5E5EF)
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("PlanToolbar"));
    toolbar->setStyleSheet(QStringLiteral("QWidget#PlanToolbar { background-color: #D5E5EF; border-bottom: 1px solid #B5C5D2; }"));
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(6, 2, 6, 2);
    tb->setSpacing(4);

    auto make_btn = [&](const QString& text) {
        auto* b = new QToolButton(toolbar);
        b->setText(text);
        b->setAutoRaise(true);
        tb->addWidget(b);
        return b;
    };

    find_btn_        = make_btn(QStringLiteral("Find Node"));
    zoom_in_btn_     = make_btn(QStringLiteral("Zoom In"));
    zoom_out_btn_    = make_btn(QStringLiteral("Zoom Out"));
    zoom_fit_btn_    = make_btn(QStringLiteral("Zoom to Fit"));
    save_btn_        = make_btn(QStringLiteral("Save Plan As…"));
    new_tab_btn_     = make_btn(QStringLiteral("Open in New Tab"));
    new_tab_btn_->setVisible(allow_open_in_new_tab_);
    missing_idx_btn_ = make_btn(QStringLiteral("Missing Index Details…"));

    stale_label_ = new QLabel(QStringLiteral("Plan may be stale (SQL edited)."), toolbar);
    stale_label_->setStyleSheet(QStringLiteral("color: #814600; font-weight: bold; padding-left: 8px;"));
    stale_label_->setVisible(false);
    tb->addWidget(stale_label_);
    tb->addStretch(1);
    root->addWidget(toolbar);

    connect(find_btn_, &QToolButton::clicked, this, &ExecutionPlanWidget::open_find_bar);
    connect(zoom_in_btn_, &QToolButton::clicked, this, [this]() { set_zoom(zoom() * detail::kZoomStep); });
    connect(zoom_out_btn_, &QToolButton::clicked, this, [this]() { set_zoom(zoom() / detail::kZoomStep); });
    connect(zoom_fit_btn_, &QToolButton::clicked, this, &ExecutionPlanWidget::zoom_to_fit);
    connect(save_btn_, &QToolButton::clicked, this, &ExecutionPlanWidget::save_selected_plan);
    connect(new_tab_btn_, &QToolButton::clicked, this, [this]() {
        if (!statements_.empty()) emit show_in_new_tab_requested(statements_);
    });
    connect(missing_idx_btn_, &QToolButton::clicked, this, &ExecutionPlanWidget::request_missing_index_script);

    // 2. Inline Find Node bar
    find_bar_ = new QFrame(this);
    find_bar_->setObjectName(QStringLiteral("PlanFindBar"));
    find_bar_->setStyleSheet(QStringLiteral("QFrame#PlanFindBar { background-color: #E4EFF6; border-bottom: 1px solid #B5C5D2; }"));
    find_bar_->setVisible(false);
    auto* fb = new QHBoxLayout(find_bar_);
    fb->setContentsMargins(6, 3, 6, 3);
    fb->setSpacing(4);

    find_property_ = new QComboBox(find_bar_);
    find_property_->setMinimumWidth(160);
    find_comparison_ = new QComboBox(find_bar_);
    find_comparison_->addItems({QStringLiteral("Contains"), QStringLiteral("="), QStringLiteral(">"), QStringLiteral("<")});
    find_value_ = new QLineEdit(find_bar_);
    find_value_->setPlaceholderText(QStringLiteral("Value…"));
    find_value_->setMinimumWidth(150);
    auto* prev_btn = new QToolButton(find_bar_);
    prev_btn->setText(QStringLiteral("◀"));
    auto* next_btn = new QToolButton(find_bar_);
    next_btn->setText(QStringLiteral("▶"));
    auto* close_btn = new QToolButton(find_bar_);
    close_btn->setText(QStringLiteral("✕"));
    find_result_ = new QLabel(find_bar_);

    fb->addWidget(find_property_);
    fb->addWidget(find_comparison_);
    fb->addWidget(find_value_);
    fb->addWidget(prev_btn);
    fb->addWidget(next_btn);
    fb->addWidget(close_btn);
    fb->addWidget(find_result_, 1);
    root->addWidget(find_bar_);

    connect(prev_btn, &QToolButton::clicked, this, [this]() { (void)find_next_node(false); });
    connect(next_btn, &QToolButton::clicked, this, [this]() { (void)find_next_node(true); });
    connect(close_btn, &QToolButton::clicked, this, &ExecutionPlanWidget::close_find_bar);
    connect(find_value_, &QLineEdit::returnPressed, this, [this]() { (void)find_next_node(true); });

    // 3. Placeholder hint when no plan is loaded
    placeholder_label_ = new QLabel(
        QStringLiteral("Use Estimated Plan (Ctrl+L) or Analyze Plan, or enable Include Actual Execution Plan (Ctrl+M) before executing."),
        this);
    placeholder_label_->setWordWrap(true);
    placeholder_label_->setContentsMargins(10, 10, 10, 10);
    root->addWidget(placeholder_label_);

    // 4. Splitter: Graphical Plan Canvas (3*) | Properties Grid (1*)
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setHandleWidth(theme::kSplitterWidthPx);
    splitter_->setVisible(false);

    scroll_area_ = new QScrollArea(splitter_);
    scroll_area_->setWidgetResizable(false);
    scroll_area_->setFrameShape(QFrame::NoFrame);
    canvas_ = new detail::PlanDiagramCanvas(scroll_area_);
    canvas_->on_select = [this](const core::ExecutionPlanOperator* op, const core::ExecutionPlanStatement* st) {
        on_operator_selected(op, st);
    };
    canvas_->on_context_menu = [this](const QPoint& global_pos) {
        QMenu menu(this);
        const bool has = !statements_.empty();
        QAction* act_save = menu.addAction(QStringLiteral("Save Execution Plan As…"));
        act_save->setEnabled(has);
        connect(act_save, &QAction::triggered, this, &ExecutionPlanWidget::save_selected_plan);

        if (allow_open_in_new_tab_) {
            QAction* act_new = menu.addAction(QStringLiteral("Show Execution Plan In New Tab"));
            act_new->setEnabled(has);
            connect(act_new, &QAction::triggered, this, [this]() {
                if (!statements_.empty()) emit show_in_new_tab_requested(statements_);
            });
        }

        QAction* act_find = menu.addAction(QStringLiteral("Find Node"));
        act_find->setEnabled(has);
        connect(act_find, &QAction::triggered, this, &ExecutionPlanWidget::open_find_bar);

        QAction* act_mi = menu.addAction(QStringLiteral("Missing Index Details…"));
        act_mi->setEnabled(!active_missing_indexes().empty());
        connect(act_mi, &QAction::triggered, this, &ExecutionPlanWidget::request_missing_index_script);

        menu.addSeparator();
        QAction* act_zi = menu.addAction(QStringLiteral("Zoom In"));
        act_zi->setEnabled(has);
        connect(act_zi, &QAction::triggered, this, [this]() { set_zoom(zoom() * detail::kZoomStep); });

        QAction* act_zo = menu.addAction(QStringLiteral("Zoom Out"));
        act_zo->setEnabled(has);
        connect(act_zo, &QAction::triggered, this, [this]() { set_zoom(zoom() / detail::kZoomStep); });

        QAction* act_zc = menu.addAction(QStringLiteral("Custom Zoom…"));
        act_zc->setEnabled(has);
        connect(act_zc, &QAction::triggered, this, &ExecutionPlanWidget::open_custom_zoom_dialog);

        QAction* act_zf = menu.addAction(QStringLiteral("Zoom to Fit"));
        act_zf->setEnabled(has);
        connect(act_zf, &QAction::triggered, this, &ExecutionPlanWidget::zoom_to_fit);

        menu.exec(global_pos);
    };
    scroll_area_->setWidget(canvas_);
    splitter_->addWidget(scroll_area_);

    properties_table_ = new QTableWidget(0, 2, splitter_);
    properties_table_->setObjectName(QStringLiteral("PlanPropertiesTable"));
    properties_table_->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    properties_table_->verticalHeader()->setVisible(false);
    properties_table_->verticalHeader()->setDefaultSectionSize(22);
    properties_table_->horizontalHeader()->setStretchLastSection(true);
    properties_table_->horizontalHeader()->resizeSection(0, 145);
    properties_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    properties_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    properties_table_->setMinimumWidth(140);
    splitter_->addWidget(properties_table_);

    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({660, 240});
    root->addWidget(splitter_, 1);
}

void ExecutionPlanWidget::set_statements(
    std::vector<core::ExecutionPlanStatement> statements,
    const QString& banner_note) {
    statements_    = std::move(statements);
    selected_op_   = nullptr;
    selected_stmt_ = nullptr;
    stale_label_->setVisible(false);

    if (statements_.empty()) {
        canvas_->set_statements(nullptr);
        properties_table_->setRowCount(0);
        splitter_->setVisible(false);
        placeholder_label_->setText(banner_note.isEmpty()
            ? QStringLiteral("No execution plan was returned.")
            : banner_note);
        placeholder_label_->setVisible(true);
        update_action_states();
        emit selection_changed();
        return;
    }

    placeholder_label_->setVisible(!banner_note.isEmpty());
    if (!banner_note.isEmpty()) {
        placeholder_label_->setText(banner_note);
    }
    splitter_->setVisible(true);
    canvas_->set_statements(&statements_);
    canvas_->set_zoom(1.0);
    on_operator_selected(&statements_[0].root, &statements_[0]);
    update_action_states();
}

void ExecutionPlanWidget::set_placeholder_message(const QString& message) {
    statements_.clear();
    selected_op_   = nullptr;
    selected_stmt_ = nullptr;
    canvas_->set_statements(nullptr);
    properties_table_->setRowCount(0);
    splitter_->setVisible(false);
    stale_label_->setVisible(false);
    placeholder_label_->setText(message);
    placeholder_label_->setVisible(true);
    update_action_states();
    emit selection_changed();
}

void ExecutionPlanWidget::set_stale(bool stale) {
    stale_label_->setVisible(stale && !statements_.empty());
}

void ExecutionPlanWidget::clear() {
    set_placeholder_message(
        QStringLiteral("Use Estimated Plan (Ctrl+L) or Analyze Plan, or enable Include Actual Execution Plan (Ctrl+M) before executing."));
}

int ExecutionPlanWidget::operator_count() const {
    return canvas_->operator_count();
}

double ExecutionPlanWidget::zoom() const noexcept {
    return canvas_->zoom();
}

void ExecutionPlanWidget::set_zoom(double factor) {
    canvas_->set_zoom(factor);
}

void ExecutionPlanWidget::zoom_to_fit() {
    if (statements_.empty()) return;
    const QSizeF logical = canvas_->logical_size();
    const QSize vp = scroll_area_->viewport()->size();
    if (logical.width() <= 1.0 || vp.width() <= 32) return;
    double fit = static_cast<double>(vp.width() - 24) / logical.width();
    if (logical.height() > 1.0 && vp.height() > 64) {
        fit = (std::min)(fit, static_cast<double>(vp.height() - 24) / logical.height());
    }
    set_zoom(fit);
}

void ExecutionPlanWidget::open_find_bar() {
    if (statements_.empty()) return;
    const QString prev = find_property_->currentText();
    const auto ops = core::collect_plan_operators(statements_);
    const auto names = core::collect_plan_property_names(ops);
    find_property_->clear();
    for (const std::string& n : names) {
        find_property_->addItem(QString::fromStdString(n));
    }
    int idx = find_property_->findText(prev);
    if (idx < 0) idx = find_property_->findText(QStringLiteral("Node Type"));
    if (idx >= 0) find_property_->setCurrentIndex(idx);
    find_result_->clear();
    find_bar_->setVisible(true);
    find_value_->setFocus();
    find_value_->selectAll();
}

void ExecutionPlanWidget::close_find_bar() {
    find_bar_->setVisible(false);
}

bool ExecutionPlanWidget::is_find_bar_visible() const {
    return find_bar_->isVisible();
}

bool ExecutionPlanWidget::find_next_node(bool forward) {
    if (statements_.empty() || find_property_->currentText().isEmpty()) {
        return false;
    }
    const auto ops = core::collect_plan_operators(statements_);
    const auto cmp = static_cast<core::ExecutionPlanComparison>((std::max)(0, find_comparison_->currentIndex()));
    const core::ExecutionPlanOperator* hit = core::find_next_plan_operator(
        ops,
        selected_op_,
        find_property_->currentText().toStdString(),
        cmp,
        find_value_->text().toStdString(),
        forward);
    if (hit == nullptr) {
        find_result_->setText(QStringLiteral("No matching node."));
        return false;
    }
    find_result_->clear();
    // Locate owning statement
    const core::ExecutionPlanStatement* owner = &statements_[0];
    for (const auto& st : statements_) {
        std::vector<core::ExecutionPlanStatement> single{st};
        // Check address range by DFS inside real statements_
        std::function<bool(const core::ExecutionPlanOperator&)> has_op = [&](const core::ExecutionPlanOperator& cur) {
            if (&cur == hit) return true;
            for (const auto& c : cur.children) {
                if (has_op(c)) return true;
            }
            return false;
        };
        if (has_op(st.root)) {
            owner = &st;
            break;
        }
    }
    on_operator_selected(hit, owner);
    const QRectF r = canvas_->rect_for_operator(hit);
    if (r.isValid()) {
        scroll_area_->ensureVisible(static_cast<int>(r.center().x()), static_cast<int>(r.center().y()), 60, 60);
    }
    return true;
}

void ExecutionPlanWidget::on_operator_selected(
    const core::ExecutionPlanOperator* op,
    const core::ExecutionPlanStatement* stmt) {
    selected_op_   = op;
    selected_stmt_ = stmt;
    canvas_->set_selected(op);
    populate_properties(op);
    update_action_states();
    emit selection_changed();
}

void ExecutionPlanWidget::populate_properties(const core::ExecutionPlanOperator* op) {
    properties_table_->setRowCount(0);
    if (op == nullptr) return;
    properties_table_->setRowCount(static_cast<int>(op->properties.size()));
    for (int i = 0; i < static_cast<int>(op->properties.size()); ++i) {
        const auto& [k, v] = op->properties[static_cast<size_t>(i)];
        properties_table_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(k)));
        properties_table_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(v)));
    }
}

void ExecutionPlanWidget::open_custom_zoom_dialog() {
    bool ok = false;
    const int cur_pct = static_cast<int>(std::round(zoom() * 100.0));
    const int val = QInputDialog::getInt(this, QStringLiteral("Custom Zoom"), QStringLiteral("Zoom (%):"), cur_pct, 10, 400, 10, &ok);
    if (ok) {
        set_zoom(static_cast<double>(val) / 100.0);
    }
}

std::vector<core::PlanIndexSuggestion> ExecutionPlanWidget::active_missing_indexes() const {
    if (selected_stmt_ != nullptr && !selected_stmt_->missing_indexes.empty()) {
        return selected_stmt_->missing_indexes;
    }
    for (const auto& st : statements_) {
        if (!st.missing_indexes.empty()) {
            return st.missing_indexes;
        }
    }
    return {};
}

void ExecutionPlanWidget::save_selected_plan() {
    const core::ExecutionPlanStatement* st = selected_stmt_ ? selected_stmt_ : (statements_.empty() ? nullptr : &statements_[0]);
    if (st == nullptr) return;
    const bool is_xml = (st->source.source_kind == core::PlanSourceKind::SqlServerXml);
    const QString filter = is_xml
        ? QStringLiteral("SQL Server Execution Plan (*.sqlplan *.xml);;All files (*.*)")
        : QStringLiteral("PostgreSQL Execution Plan (*.json);;All files (*.*)");
    const QString path = save_file_picker_
        ? save_file_picker_(filter)
        : QFileDialog::getSaveFileName(
              this,
              QStringLiteral("Save Execution Plan As"),
              is_xml ? QStringLiteral("plan.sqlplan") : QStringLiteral("plan.json"),
              filter);
    if (path.isEmpty()) return;

    const auto res = core::save_raw_execution_plan(st->source, std::filesystem::path(path.toStdWString()));
    if (!res) {
        emit status_message(QStringLiteral("Plan export failed: %1").arg(QString::fromStdString(res.error())));
    } else {
        emit status_message(is_xml
            ? QStringLiteral("Execution plan saved as SQL Server ShowPlan XML.")
            : QStringLiteral("Execution plan saved as PostgreSQL JSON."));
    }
}

void ExecutionPlanWidget::request_missing_index_script() {
    const auto suggestions = active_missing_indexes();
    if (suggestions.empty()) return;
    QString script;
    for (const auto& s : suggestions) {
        if (!script.isEmpty()) script += QLatin1Char('\n');
        script += QString::fromStdString(s.commented_script);
    }
    emit open_script_in_new_query_requested(script);
}

void ExecutionPlanWidget::update_action_states() {
    const bool has = !statements_.empty();
    find_btn_->setEnabled(has);
    zoom_in_btn_->setEnabled(has);
    zoom_out_btn_->setEnabled(has);
    zoom_fit_btn_->setEnabled(has);
    save_btn_->setEnabled(has);
    new_tab_btn_->setEnabled(has);
    missing_idx_btn_->setEnabled(!active_missing_indexes().empty());
}

} // namespace capybaradb::ui
