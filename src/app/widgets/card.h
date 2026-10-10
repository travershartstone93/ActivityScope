#pragma once
#include <QColor>
#include <QFrame>
#include <QString>

class QVBoxLayout;

// A rounded panel with an optional icon, title, value and subtitle line.
// Tinted cards (the small resource tiles) pick up their colour in the border
// and background, the way an instrument face would.
class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(const QString& icon = {}, const QString& title = {}, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }
    void setTitle(const QString& t) { title_ = t; update(); }
    void setValue(const QString& v) { value_ = v; update(); }
    void setSubtitle(const QString& s) { sub_ = s; update(); }
    void setTint(const QColor& c) { tint_ = c; update(); }
    void setTitleSize(int px) { titlePx_ = px; updateMargins(); }
    void setHighlighted(bool h) { hi_ = h; update(); }
    void setClickable(bool c) { click_ = c; setCursor(c ? Qt::PointingHandCursor : Qt::ArrowCursor); }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    void updateMargins();
    QString icon_, title_, value_, sub_;
    QColor tint_;
    int titlePx_ = 20;
    bool hi_ = false, click_ = false;
    QVBoxLayout* body_;
};
