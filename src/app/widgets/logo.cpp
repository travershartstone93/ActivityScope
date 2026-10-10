#include "logo.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "../theme.h"

namespace {

QPainterPath trace(const QRectF& r) {
    // Flat line, a sharp spike, a ringing decay: what the app looks for.
    QPainterPath path;
    const int steps = 120;
    for (int i = 0; i <= steps; ++i) {
        double t = double(i) / steps;
        double y = 0;
        if (t > 0.30 && t < 0.36) y = -std::sin((t - 0.30) / 0.06 * M_PI) * 0.95;
        else if (t >= 0.36 && t < 0.42) y = std::sin((t - 0.36) / 0.06 * M_PI) * 0.55;
        else if (t >= 0.42) y = -std::sin((t - 0.42) * 38) * 0.22 * std::exp(-(t - 0.42) * 7);
        QPointF pt(r.left() + t * r.width(), r.center().y() + y * r.height() / 2);
        if (i == 0) path.moveTo(pt);
        else path.lineTo(pt);
    }
    return path;
}

}  // namespace

Logo::Logo(QWidget* parent) : QWidget(parent) { setFixedHeight(70); }

void Logo::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette& P = pal();
    QColor c = P.phosphor ? P.cpu : P.gpu.lighter(115);
    QRectF tr(12, 8, 64, 54);
    QPainterPath path = trace(tr);
    for (auto [w, a] : {std::pair{9.0, 30}, {5.0, 70}, {2.2, 255}}) {
        p.setPen(QPen(withAlpha(c, a), w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }
    QFont f = Theme::get().heading(19);
    f.setWeight(QFont::Bold);
    p.setFont(f);
    QRectF text(84, 10, width() - 90, 28);
    for (int a : {40, 90}) {
        p.setPen(withAlpha(c, a));
        p.drawText(text.translated(0, a == 40 ? 1.5 : 0.7), Qt::AlignLeft | Qt::AlignVCenter, "Activity");
    }
    p.setPen(c.lighter(125));
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, "Activity");
    p.setPen(P.text);
    p.drawText(text.translated(0, 24), Qt::AlignLeft | Qt::AlignVCenter, "Scope");
}

QPixmap Logo::trayPixmap(const QColor& color, bool alert) {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#16181d"));
    p.setPen(QPen(alert ? QColor("#f43f5e") : color, 3));
    p.drawRoundedRect(QRectF(3, 3, 58, 58), 12, 12);
    QPainterPath path = trace(QRectF(8, 12, 48, 40));
    p.setPen(QPen(alert ? QColor("#f43f5e") : color, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    return pm;
}
