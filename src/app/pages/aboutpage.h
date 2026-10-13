#pragma once
#include <QWidget>

// What this program is, everything it can do, and the rules it lives by.
class AboutPage : public QWidget {
    Q_OBJECT
public:
    explicit AboutPage(QWidget* parent = nullptr);
    void refresh() {}
};
