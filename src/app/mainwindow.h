#pragma once
#include <QMainWindow>
#include <QSystemTrayIcon>

#include "recorder.h"

class QLabel;
class QStackedWidget;
class QButtonGroup;
class QComboBox;
class SummaryPage;
class PerformancePage;
class ProcessesPage;
class HistoryPage;
class SettingsPage;
class SystemInfoPage;
class LogPage;
class WeirdPage;
class AboutPage;
class UpdatesPage;
class StartupPage;
class UsersPage;
class ServicesPage;
class ConnectionsPage;
class InstalledPage;
class DiskSpacePage;
class PowerPage;
class Popout;
class TrayWatch;
class Notifier;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Recorder* recorder);
    void showFromTray();
    void togglePopout();
    void openPage(int index);

signals:
    void visibilityChanged(bool visible);

protected:
    void closeEvent(QCloseEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    void build();
    void onSample(const as::SnapshotPtr& s);
    void onOpenEvent(const EventPtr& e, bool open);
    void refreshVisible();
    void updateTray();

    Recorder* recorder_;
    Notifier* notifier_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QButtonGroup* nav_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* eventDot_ = nullptr;
    SummaryPage* summary_ = nullptr;
    PerformancePage* performance_ = nullptr;
    ProcessesPage* processes_ = nullptr;
    HistoryPage* history_ = nullptr;
    SettingsPage* settings_ = nullptr;
    SystemInfoPage* sysinfo_ = nullptr;
    LogPage* log_ = nullptr;
    WeirdPage* weird_ = nullptr;
    AboutPage* about_ = nullptr;
    UpdatesPage* updates_ = nullptr;
    StartupPage* startup_ = nullptr;
    UsersPage* users_ = nullptr;
    ServicesPage* services_ = nullptr;
    ConnectionsPage* connections_ = nullptr;
    InstalledPage* installed_ = nullptr;
    DiskSpacePage* disk_ = nullptr;
    PowerPage* power_ = nullptr;
    Popout* popout_ = nullptr;
    TrayWatch* trayWatch_ = nullptr;
    int page_ = 0;
    bool trayAlert_ = false;
    bool trayHot_ = false;
    bool quitting_ = false;
};
