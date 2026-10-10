#pragma once
#include <QColor>
#include <QWidget>

// A column or row of LED segments that light up with the value, with a glow.
class SegmentMeter : public QWidget {
    Q_OBJECT
public:
    explicit SegmentMeter(Qt::Orientation o = Qt::Vertical, QWidget* parent = nullptr);
    void setValue(double fraction);       // 0..1; negative means unavailable
    void setColor(const QColor& c);
    void setHeat(bool on) { heat_ = on; update(); }  // green-to-red ramp along the meter
    void setSegmentSize(int px) { seg_ = px; update(); }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Qt::Orientation o_;
    double v_ = -1;
    QColor color_;
    bool heat_ = false;
    int seg_ = 9;
};
