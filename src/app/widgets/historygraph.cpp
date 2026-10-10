#include "historygraph.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "../theme.h"

HistoryGraph::HistoryGraph(QWidget* parent) : QWidget(parent) {
    setMinimumSize(80, 40);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void HistoryGraph::setSeries(const QVector<GraphSeries>& s) {
    s_ = s;
    if (auto_) {
        double m = floor_;
        for (const auto& g : s_)
            for (double v : g.values)
                if (!std::isnan(v)) m = std::max(m, v);
        hi_ = m * 1.12;
        lo_ = 0;
    }
    update();
}

QRectF HistoryGraph::plotRect() const {
    QFontMetrics fm(Theme::get().body(11));
    double left = at_.isEmpty() ? 1 : fm.horizontalAdvance(at_.size() > ab_.size() ? at_ : ab_) + 10;
    double right = rt_.isEmpty() ? 1 : fm.horizontalAdvance(rt_) + 10;
    return QRectF(left, 1, width() - left - right - 1, height() - 2);
}

void HistoryGraph::mousePressEvent(QMouseEvent* e) {
    QRectF r = plotRect();
    if (!r.contains(e->position())) return;
    double idx = (e->position().x() - r.left()) / r.width() * (cap_ - 1);
    emit clicked(idx);
}

void HistoryGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette& P = pal();
    QColor frame = frame_.isValid() ? frame_ : (s_.isEmpty() ? P.border : s_.first().color);
    QRectF r = plotRect();
    QFont small = Theme::get().body(11);
    p.setFont(small);

    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(frame, P.phosphor ? 16 : 12));
    p.drawRoundedRect(r, 4, 4);

    p.setPen(QPen(withAlpha(frame, 28), 1));
    for (int i = 1; i < gc_; ++i) {
        double x = r.left() + r.width() * i / gc_;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    for (int i = 1; i < gr_; ++i) {
        double y = r.top() + r.height() * i / gr_;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    if (!compact_) {
        p.setPen(QPen(withAlpha(frame, 16), 1));
        for (int i = 1; i < gc_ * 4; ++i) {
            if (i % 4 == 0) continue;
            double x = r.left() + r.width() * i / (gc_ * 4);
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }

    auto xAt = [&](double i) { return r.left() + r.width() * i / std::max(1, cap_ - 1); };
    auto yAt = [&](double v) {
        double t = (v - lo_) / std::max(1e-9, hi_ - lo_);
        return r.bottom() - std::clamp(t, 0.0, 1.0) * r.height();
    };

    for (const auto& [a, b] : spans_) {
        QRectF span(QPointF(xAt(a), r.top()), QPointF(xAt(b), r.bottom()));
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(P.alert, 34));
        p.drawRect(span);
        p.setPen(QPen(withAlpha(P.alert, 120), 1));
        p.drawLine(span.topLeft(), span.bottomLeft());
    }

    p.save();
    p.setClipRect(r.adjusted(-1, -1, 1, 1));
    for (const auto& g : s_) {
        const int n = g.values.size();
        const int offset = cap_ - n;  // newest sample sits at the right edge
        QVector<QPainterPath> runs;
        QPainterPath cur;
        bool open = false;
        for (int i = 0; i < n; ++i) {
            double v = g.values[i];
            if (std::isnan(v)) {
                if (open) runs.push_back(cur);
                cur = QPainterPath();
                open = false;
                continue;
            }
            QPointF pt(xAt(offset + i), yAt(v));
            if (!open) {
                cur.moveTo(pt);
                open = true;
            } else {
                cur.lineTo(pt);
            }
        }
        if (open) runs.push_back(cur);
        for (const auto& run : runs) {
            if (g.fill && run.elementCount() > 1) {
                QPainterPath area = run;
                area.lineTo(run.currentPosition().x(), r.bottom());
                area.lineTo(run.elementAt(0).x, r.bottom());
                area.closeSubpath();
                QLinearGradient grad(0, r.top(), 0, r.bottom());
                grad.setColorAt(0, withAlpha(g.color, 95));
                grad.setColorAt(1, withAlpha(g.color, 30));
                p.fillPath(area, grad);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(withAlpha(g.color, 32), g.width + 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(run);
            p.setPen(QPen(withAlpha(g.color, 70), g.width + 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(run);
            p.setPen(QPen(g.color.lighter(112), g.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(run);
        }
    }
    p.restore();

    if (marker_ >= 0) {
        p.setPen(QPen(P.text, 1.5, Qt::DashLine));
        p.drawLine(QPointF(xAt(marker_), r.top()), QPointF(xAt(marker_), r.bottom()));
    }

    // End markers: a small pointer at each line's newest value.
    for (const auto& g : s_) {
        if (g.values.isEmpty() || std::isnan(g.values.last())) continue;
        double y = yAt(g.values.last());
        QPainterPath tri;
        tri.moveTo(r.right() - 1, y);
        tri.lineTo(r.right() + 5, y - 5);
        tri.lineTo(r.right() + 5, y + 5);
        tri.closeSubpath();
        p.fillPath(tri, P.text);
    }

    p.setPen(QPen(withAlpha(frame, 140), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 4, 4);

    p.setPen(withAlpha(frame, 230));
    if (!tl_.isEmpty()) p.drawText(r.adjusted(7, 4, -7, -4), Qt::AlignLeft | Qt::AlignTop, tl_);
    if (!bl_.isEmpty()) p.drawText(r.adjusted(7, 4, -7, -4), Qt::AlignLeft | Qt::AlignBottom, bl_);
    p.setPen(P.dim);
    QRectF la(0, r.top(), r.left() - 6, r.height());
    if (!at_.isEmpty()) p.drawText(la, Qt::AlignRight | Qt::AlignTop, at_);
    if (!am_.isEmpty()) p.drawText(la, Qt::AlignRight | Qt::AlignVCenter, am_);
    if (!ab_.isEmpty()) p.drawText(la, Qt::AlignRight | Qt::AlignBottom, ab_);
    QRectF ra(r.right() + 8, r.top(), width() - r.right() - 8, r.height());
    if (!rt_.isEmpty()) p.drawText(ra, Qt::AlignLeft | Qt::AlignTop, rt_);
    if (!rm_.isEmpty()) p.drawText(ra, Qt::AlignLeft | Qt::AlignVCenter, rm_);
    if (!rb_.isEmpty()) p.drawText(ra, Qt::AlignLeft | Qt::AlignBottom, rb_);
}
