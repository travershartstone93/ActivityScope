#pragma once
#include <QString>
#include <QWidget>

#include "snapshot.h"

class Card;
class HistoryGraph;
class SegmentMeter;
class QLabel;
class TopList;

class SummaryPage : public QWidget {
    Q_OBJECT
public:
    explicit SummaryPage(QWidget* parent = nullptr);
    void refresh();

signals:
    void openHistory();

private:
    Card* banner_;
    QLabel* bannerText_;
    Card* heat_;
    QLabel* heatText_;
    class QPushButton* heatButton_;
    as::ProcessKey heatTarget_{};
    QString heatName_;
    SegmentMeter *mCpu_, *mClock_, *mTemp_, *mGpu_, *mMem_;
    QLabel *lCpu_, *lClock_, *lTemp_, *lGpu_, *lMem_;
    Card *cpuCard_, *topCard_, *memCard_;
    HistoryGraph *cpuGraph_, *memGraph_;
    QLabel *cpuFootL_, *cpuFootR_, *memFootL_, *memFootM_, *memFootR_;
    TopList* top_;
    Card *disks_, *net_, *energy_, *gpu_, *thermal_, *fan_;
    SegmentMeter *bDisks_, *bNet_, *bEnergy_, *bGpu_, *bThermal_, *bFan_;
};
