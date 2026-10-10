#pragma once
#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

struct GraphSeries {
    QVector<double> values;   // oldest first; NaN leaves a gap
    QColor color;
    double width = 2.0;
    bool fill = false;
};

// A gridded history plot: glowing lines, gaps for missing samples, an end
// marker at the newest value, and optional shaded spans for events.
class HistoryGraph : public QWidget {
    Q_OBJECT
public:
    explicit HistoryGraph(QWidget* parent = nullptr);
    void setSeries(const QVector<GraphSeries>& s);
    void setRange(double lo, double hi) { lo_ = lo; hi_ = hi; auto_ = false; update(); }
    void setAutoRange(double floor) { auto_ = true; floor_ = floor; update(); }
    void setCapacity(int n) { cap_ = n; update(); }
    void setFrameColor(const QColor& c) { frame_ = c; update(); }
    void setCornerText(const QString& topLeft, const QString& bottomLeft = {}) { tl_ = topLeft; bl_ = bottomLeft; update(); }
    void setAxisText(const QString& top, const QString& mid, const QString& bottom) { at_ = top; am_ = mid; ab_ = bottom; update(); }
    void setRightAxisText(const QString& top, const QString& mid, const QString& bottom) { rt_ = top; rm_ = mid; rb_ = bottom; update(); }
    void setSpans(const QVector<QPair<double, double>>& spans) { spans_ = spans; update(); }  // index ranges
    void setMarker(double index) { marker_ = index; update(); }
    void setGrid(int cols, int rows) { gc_ = cols; gr_ = rows; update(); }
    void setCompact(bool c) { compact_ = c; update(); }
    double max() const { return hi_; }

signals:
    void clicked(double index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QRectF plotRect() const;
    QVector<GraphSeries> s_;
    double lo_ = 0, hi_ = 100, floor_ = 1;
    bool auto_ = false, compact_ = false;
    int cap_ = 120, gc_ = 12, gr_ = 4;
    QColor frame_;
    QString tl_, bl_, at_, am_, ab_, rt_, rm_, rb_;
    QVector<QPair<double, double>> spans_;
    double marker_ = -1;
};
