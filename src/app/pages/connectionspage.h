#pragma once
#include <QWidget>

class QTreeWidget;
class QCheckBox;
class QLineEdit;
class QLabel;

class ConnectionsPage : public QWidget {
    Q_OBJECT
public:
    explicit ConnectionsPage(QWidget* parent = nullptr);
    void refresh();

private:
    void reload();
    QTreeWidget* tree_;
    QCheckBox* listening_;
    QLineEdit* filter_;
    QLabel* count_;
    double loaded_ = 0;
};
