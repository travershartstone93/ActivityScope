#pragma once
#include <QWidget>

#include "events.h"

class Recorder;
class QLabel;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(Recorder* r, QWidget* parent = nullptr);
    static as::Triggers savedTriggers();
    static void applySaved(Recorder* r);

private:
    void setLoginStart(bool on);
    Recorder* recorder_;
    QLabel* loginMsg_;
    QLabel* dbInfo_;
};
