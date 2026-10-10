#include "card.h"

#include <QPainter>
#include <QVBoxLayout>

#include "../icons.h"
#include "../theme.h"

Card::Card(const QString& icon, const QString& title, QWidget* parent) : QFrame(parent), icon_(icon), title_(title) {
    body_ = new QVBoxLayout(this);
    body_->setSpacing(6);
    updateMargins();
}

void Card::updateMargins() {
    int top = title_.isEmpty() && icon_.isEmpty() ? 12 : titlePx_ + 22;
    body_->setContentsMargins(14, top + (sub_.isEmpty() ? 0 : 18), 14, 12);
    update();
}

void Card::mousePressEvent(QMouseEvent*) {
    if (click_) emit clicked();
}

void Card::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette& P = pal();
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor bg = hi_ ? P.panelHi : P.panel;
    if (tint_.isValid()) {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, QColor::fromRgbF(bg.redF() * 0.8 + tint_.redF() * 0.2, bg.greenF() * 0.8 + tint_.greenF() * 0.2,
                                         bg.blueF() * 0.8 + tint_.blueF() * 0.2));
        g.setColorAt(1, QColor::fromRgbF(bg.redF() * 0.9 + tint_.redF() * 0.1, bg.greenF() * 0.9 + tint_.greenF() * 0.1,
                                         bg.blueF() * 0.9 + tint_.blueF() * 0.1));
        p.setBrush(g);
        p.setPen(QPen(withAlpha(tint_, 120), 1.2));
    } else {
        p.setBrush(bg);
        p.setPen(QPen(hi_ ? P.selection.lighter(140) : P.border, 1));
    }
    p.drawRoundedRect(r, 9, 9);

    if (title_.isEmpty() && icon_.isEmpty()) return;
    int x = 14;
    QColor ic = tint_.isValid() ? tint_ : P.dim;
    if (!icon_.isEmpty()) {
        int sz = titlePx_ + 2;
        p.drawPixmap(x, 13 + (titlePx_ + 6 - sz) / 2, iconPixmap(icon_, ic, sz));
        x += sz + 10;
    }
    p.setPen(P.text);
    p.setFont(Theme::get().heading(titlePx_));
    QRect titleRect(x, 10, width() - x - 14, titlePx_ + 10);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title_);
    if (!value_.isEmpty()) {
        p.setPen(tint_.isValid() ? tint_.lighter(115) : P.cpu);
        p.setFont(Theme::get().body(titlePx_ - 3, QFont::Bold));
        p.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter, value_);
    }
    if (!sub_.isEmpty()) {
        p.setPen(P.dim);
        p.setFont(Theme::get().body(13));
        p.drawText(QRect(14, titleRect.bottom() + 2, width() - 28, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(sub_, Qt::ElideRight, width() - 28));
    }
}
