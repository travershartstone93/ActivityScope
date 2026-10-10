#pragma once
#include <QWidget>

class QTreeWidget;
class QComboBox;
class QLabel;
class QPushButton;
class QLineEdit;

class ServicesPage : public QWidget {
    Q_OBJECT
public:
    explicit ServicesPage(QWidget* parent = nullptr);
    void refresh();

private:
    void reload();
    void act(const QString& verb);
    QTreeWidget* tree_;
    QComboBox* scope_;
    QLineEdit* filter_;
    QLabel* msg_;
    QPushButton *start_, *stop_, *restart_;
    double loaded_ = 0;
};
