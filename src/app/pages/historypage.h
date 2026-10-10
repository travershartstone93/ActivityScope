#pragma once
#include <QWidget>
#include <memory>
#include <vector>

#include "store.h"

class QListWidget;
class QLabel;
class QComboBox;
class HistoryGraph;
class Timeline;
class CulpritList;
class Card;

class HistoryPage : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPage(QWidget* parent = nullptr);
    void refresh();
    void reloadEvents();

signals:
    void showProcess(int pid);

private:
    void showEvent(int64_t id);
    void showOpen();
    void showMoment(double t);
    void fill(const as::Event& e);
    void loadTimeline();

    std::unique_ptr<as::Store> store_;
    Timeline* timeline_;
    QComboBox* range_;
    QListWidget* list_;
    Card* detail_;
    QLabel *summary_, *triggers_, *meta_, *graphNote_;
    HistoryGraph* graph_;
    CulpritList* culprits_;
    std::vector<as::EventRow> events_;
    int64_t current_ = 0;
    bool showingOpen_ = false;
    double lastTimeline_ = 0;
};
