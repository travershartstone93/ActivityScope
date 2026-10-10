#pragma once
#include <QWidget>

class QTreeWidget;
class QLabel;
class QPushButton;

class StartupPage : public QWidget {
    Q_OBJECT
public:
    explicit StartupPage(QWidget* parent = nullptr);
    void refresh();

private:
    void reload();
    void toggleSelected();
    QTreeWidget* tree_;
    QPushButton* toggle_;
    QLabel* msg_;
    double loaded_ = 0;
};
