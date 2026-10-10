#pragma once
#include <QWidget>

class HistoryGraph;
class QGridLayout;
class CoreBars;

class PowerPage : public QWidget {
    Q_OBJECT
public:
    explicit PowerPage(QWidget* parent = nullptr);
    void refresh();

private:
    CoreBars* bars_;
    HistoryGraph *clock_, *draw_;
    QGridLayout* stats_;
};
