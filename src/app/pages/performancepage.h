#pragma once
#include <QWidget>
#include <memory>
#include <vector>

#include "store.h"

class Card;
class HistoryGraph;
class SegmentMeter;
class QLabel;
class QGridLayout;
class QComboBox;
class QVBoxLayout;

class PerformancePage : public QWidget {
    Q_OBJECT
public:
    explicit PerformancePage(QWidget* parent = nullptr);
    void refresh();

private:
    struct Item {
        QString id, icon, title;
        QColor color;
        Card* card = nullptr;
        HistoryGraph* mini = nullptr;
        QLabel* text = nullptr;
    };
    void ensureItems();
    void select(const QString& id);
    void updateList();
    void updateDetail();
    void loadPast();
    void setStats(const QList<QPair<QString, QString>>& rows);

    std::unique_ptr<as::Store> store_;
    QVBoxLayout* listBox_;
    std::vector<Item> items_;
    QString current_ = "cpu";
    QLabel *title_, *big_, *sub_, *caption_;
    SegmentMeter* meter_;
    QWidget* coreGrid_;
    QGridLayout* coreLayout_;
    std::vector<HistoryGraph*> coreGraphs_;
    HistoryGraph* graph_;
    QGridLayout* stats_;
    QComboBox* range_;
    std::vector<as::StoredSample> past_;
    std::vector<as::EventRow> pastEvents_;
    double pastLoaded_ = 0;
};
