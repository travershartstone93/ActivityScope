#pragma once
#include <QWidget>
#include <memory>
#include <vector>

#include "logbook.h"
#include "store.h"

class QTreeWidget;
class QLineEdit;
class QComboBox;
class QLabel;
class QButtonGroup;
class Card;

// The investigation log: everything ActivityScope has noticed, in one list.
class LogPage : public QWidget {
    Q_OBJECT
public:
    explicit LogPage(QWidget* parent = nullptr);
    void refresh();
    void reload();

private:
    void showSelected();
    void exportRange();
    std::unique_ptr<as::Store> store_;
    QTreeWidget* tree_;
    QLineEdit* search_;
    QComboBox* range_;
    QButtonGroup* cats_;
    Card* detail_;
    QLabel *dWhen_, *dText_;
    QLabel* count_;
    std::vector<as::LogEntry> rows_;
    double loaded_ = 0;
};
