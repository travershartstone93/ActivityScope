#pragma once
#include <QWidget>

class QTreeWidget;

class UsersPage : public QWidget {
    Q_OBJECT
public:
    explicit UsersPage(QWidget* parent = nullptr);
    void refresh();

private:
    QTreeWidget* tree_;
    double loaded_ = 0;
};
