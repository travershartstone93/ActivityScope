#include "segmentmeter.h"

#include <QPainter>
#include <cmath>

#include "../theme.h"

SegmentMeter::SegmentMeter(Qt::Orientation o, QWidget* parent) : QWidget(parent), o_(o), color_(pal().cpu) {
    setSizePolicy(o == Qt::Vertical ? QSizePolicy::Fixed : QSizePolicy::Expanding,
                  o == Qt::Vertical ? QSizePolicy::Expanding : QSizePolicy::Fixed);
}

QSize SegmentMeter::sizeHint() const { return o_ == Qt::Vertical ? QSize(42, 200) : QSize(300, 22); }

void SegmentMeter::setValue(double f) {
    if (std::abs(f - v_) < 1e-4) return;
    v_ = f;
    update();
}

void SegmentMeter::setColor(const QColor& c) {
    color_ = c;
    update();
}

void SegmentMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool vert = o_ == Qt::Vertical;
    const int len = vert ? height() : width();
    const int thick = vert ? width() : height();
    const int gap = std::max(3, seg_ / 3);
    const int n = std::max(1, (len + gap) / (seg_ + gap));
    const double used = n * seg_ + (n - 1) * gap;
    const double off = (len - used) / 2.0;
    const int lit = v_ < 0 ? 0 : int(std::round(std::clamp(v_, 0.0, 1.0) * n));
    const double pad = thick * 0.14;

    for (int i = 0; i < n; ++i) {
        double pos = off + i * (seg_ + gap);
        QRectF r = vert ? QRectF(pad, len - pos - seg_, thick - 2 * pad, seg_) : QRectF(pos, pad, seg_, thick - 2 * pad);
        QColor c = color_;
        if (heat_ && !pal().phosphor) {
            double t = double(i) / std::max(1, n - 1);
            c = t < 0.55 ? pal().cpu : t < 0.8 ? pal().temp : pal().alert;
        }
        if (i < lit) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(c, 45));
            p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), 4, 4);
            p.setBrush(c);
            p.drawRoundedRect(r, 2.5, 2.5);
            p.setBrush(withAlpha(Qt::white, 70));
            QRectF shine = vert ? QRectF(r.x() + 2, r.y() + 1.5, r.width() - 4, 2) : QRectF(r.x() + 1.5, r.y() + 2, 2, r.height() - 4);
            p.drawRoundedRect(shine, 1, 1);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(c, v_ < 0 ? 18 : 34));
            p.drawRoundedRect(r, 2.5, 2.5);
        }
    }
}
