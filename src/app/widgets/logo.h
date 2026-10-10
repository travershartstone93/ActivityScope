#pragma once
#include <QPixmap>
#include <QWidget>

// The ActivityScope mark: an oscilloscope trace with a spike, and the name.
class Logo : public QWidget {
    Q_OBJECT
public:
    explicit Logo(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {230, 70}; }
    static QPixmap trayPixmap(const QColor& color, bool alert);

protected:
    void paintEvent(QPaintEvent*) override;
};
