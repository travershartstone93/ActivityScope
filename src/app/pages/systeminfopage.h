#pragma once
#include <QWidget>

class QGridLayout;

class SystemInfoPage : public QWidget {
    Q_OBJECT
public:
    explicit SystemInfoPage(QWidget* parent = nullptr);
    void refresh();

private:
    QGridLayout* grid_;
    QString text_;
    double built_ = 0;
};
