#pragma once
#include <QWidget>

#include "events.h"

class Recorder;
class QLabel;
class QPushButton;
class QShowEvent;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(Recorder* r, QWidget* parent = nullptr);
    static as::Triggers savedTriggers();
    static void applySaved(Recorder* r);

private:
    void setLoginStart(bool on);
    Recorder* recorder_;
    void showEvent(QShowEvent* e) override;
    void refreshAllowed();
    QLabel* loginMsg_;
    QLabel* allowedLbl_ = nullptr;
    QPushButton* resetAllowed_ = nullptr;
    QLabel* dbInfo_;
};
