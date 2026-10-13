#include "mainwindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include "format.h"
#include "icons.h"
#include "live.h"
#include "notifier.h"
#include "traywatch.h"
#include "pages/historypage.h"
#include "pages/performancepage.h"
#include "pages/processespage.h"
#include "pages/settingspage.h"
#include "pages/summarypage.h"
#include "pages/logpage.h"
#include "pages/weirdpage.h"
#include "pages/updatespage.h"
#include "pages/connectionspage.h"
#include "pages/diskspacepage.h"
#include "pages/installedpage.h"
#include "pages/powerpage.h"
#include "pages/servicespage.h"
#include "pages/startuppage.h"
#include "pages/systeminfopage.h"
#include "pages/userspage.h"
#include "widgets/popout.h"
#include "theme.h"
#include "widgets/logo.h"

namespace {

// Order matches the stack: the first four are the core, then the system
// pages, Settings last.
const struct { const char* icon; const char* label; } kPages[] = {
    {"layout-dashboard", "Summary"}, {"activity", "Performance"}, {"list-tree", "Processes"}, {"server", "History"},
    {"scroll-text", "Log"},          {"search", "Weird processes"},   {"info", "System info"},         {"power", "Startup apps"},   {"users", "Users"},         {"cog", "Services"},
    {"cable", "Connections"},        {"package", "Installed apps"}, {"download", "Updates"},     {"chart-pie", "Disk space"},
    {"gauge", "Power & clocks"},
    {"settings", "Settings"},
};

}  // namespace

MainWindow::MainWindow(Recorder* recorder) : recorder_(recorder) {
    setWindowTitle("ActivityScope");
    resize(1500, 980);
    setMinimumSize(1100, 720);
    QSettings st;
    Theme::get().set(st.value("theme", "Dark").toString());
    SettingsPage::applySaved(recorder_);
    build();

    tray_ = new QSystemTrayIcon(this);
    auto* menu = new QMenu(this);
    menu->addAction("Open ActivityScope", this, &MainWindow::showFromTray);
    menu->addAction("History", this, [this] {
        showFromTray();
        openPage(3);
    });
    menu->addAction("Popout readout", this, &MainWindow::togglePopout);
    menu->addSeparator();
    menu->addAction("Quit (stops recording)", this, [this] {
        quitting_ = true;
        qApp->quit();
    });
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger) return;
        if (isVisible() && !isMinimized()) hide();
        else showFromTray();
    });
    updateTray();
    tray_->show();
    if (st.value("popout/open", false).toBool()) togglePopout();

    notifier_ = new Notifier(this);
    connect(notifier_, &Notifier::openRequested, this, &MainWindow::showFromTray);
    connect(recorder_, &Recorder::sharing, notifier_, &Notifier::sharing);
    // The tray watcher waits on other programs over the bus, so it gets its
    // own thread and can never stall the window.
    auto* trayThread = new QThread(this);
    trayThread->setObjectName("traywatch");
    trayWatch_ = new TrayWatch;
    trayWatch_->moveToThread(trayThread);
    connect(trayThread, &QThread::started, trayWatch_, &TrayWatch::start);
    connect(trayThread, &QThread::finished, trayWatch_, &QObject::deleteLater);
    connect(qApp, &QCoreApplication::aboutToQuit, trayThread, [trayThread] {
        trayThread->quit();
        // The watcher may legitimately be inside a 5 s D-Bus call. If it is
        // stuck longer, detach the thread rather than abort on the way out.
        if (!trayThread->wait(8000)) trayThread->setParent(nullptr);
    });
    connect(trayWatch_, &TrayWatch::logLine, this, [this](const QString& c, const QString& t, const QString& d) {
        QMetaObject::invokeMethod(recorder_, [r = recorder_, c, t, d] { r->addLog(c, t, d); }, Qt::QueuedConnection);
    });
    trayThread->start(QThread::LowPriority);
    connect(notifier_, &Notifier::allowSharer, this, [this](const QString& program) {
        QSettings st;
        QStringList allowed = st.value("allowed_sharers").toStringList();
        if (!allowed.contains(program)) allowed << program;
        st.setValue("allowed_sharers", allowed);
        SettingsPage::applySaved(recorder_);
    });
    connect(recorder_, &Recorder::sampled, this, &MainWindow::onSample);
    connect(recorder_, &Recorder::logged, this, [this](int) {
        if (log_ && page_ == 4 && isVisible()) log_->reload();
    });
    connect(recorder_, &Recorder::openEvent, this, &MainWindow::onOpenEvent);
    connect(recorder_, &Recorder::eventClosed, this, [this](qint64, const QString& summary) {
        if (history_) history_->reloadEvents();  // also recounts today's events
        if (tray_) tray_->setToolTip("ActivityScope: last event\n" + summary.left(200));
    });
    connect(&Theme::get(), &Theme::changed, this, [this] {
        int keep = page_;
        build();
        openPage(keep);
        updateTray();
    });
}

void MainWindow::build() {
    const Palette& P = pal();
    qApp->setPalette(Theme::get().palette());
    qApp->setFont(Theme::get().body(13));
    qApp->setStyleSheet(Theme::get().styleSheet());
    auto* root = new QWidget;
    root->setObjectName("root");
    root->setStyleSheet(QString("#root { background:%1; }").arg(P.window.name()));
    auto* h = new QHBoxLayout(root);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    auto* side = new QWidget;
    side->setObjectName("side");
    side->setFixedWidth(250);
    side->setStyleSheet(QString("#side { background:%1; border-right:1px solid %2; }").arg(P.sidebar.name(), P.border.name(QColor::HexArgb)));
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(12, 16, 12, 12);
    sv->setSpacing(6);
    nav_ = new QButtonGroup(this);
    nav_->setExclusive(true);
    for (int i = 0; i < int(std::size(kPages)); ++i) {
        auto* b = new QPushButton(QString("   ") + QString(kPages[i].label).replace("&", "&&"));
        b->setCheckable(true);
        b->setIcon(themedIcon(kPages[i].icon, P.text));
        b->setIconSize(QSize(20, 20));
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString("QPushButton { text-align:left; padding:8px 14px; border:none; border-radius:9px;"
                                 " font-size:15px; background:transparent; }"
                                 "QPushButton:hover { background:%1; }"
                                 "QPushButton:checked { background:%2; font-weight:600; }")
                             .arg(P.panelHi.name(QColor::HexArgb), P.selection.name(QColor::HexArgb)));
        nav_->addButton(b, i);
        sv->addWidget(b);
        if (i == 5 || i == 14) {
            auto* sep = new QFrame;
            sep->setFixedHeight(1);
            sep->setStyleSheet(QString("background:%1; margin: 0 10px;").arg(P.border.name(QColor::HexArgb)));
            sv->addSpacing(5);
            sv->addWidget(sep);
            sv->addSpacing(5);
        }
    }
    connect(nav_, &QButtonGroup::idClicked, this, &MainWindow::openPage);
    sv->addStretch();
    sv->addWidget(new Logo);
    auto* themeRow = new QHBoxLayout;
    auto* pal_icon = new QLabel;
    pal_icon->setPixmap(iconPixmap("palette", P.dim, 18));
    auto* themes = new QComboBox;
    themes->addItems(Theme::names());
    themes->setCurrentText(P.name);
    connect(themes, &QComboBox::currentTextChanged, this, [](const QString& t) {
        QSettings().setValue("theme", t);
        QMetaObject::invokeMethod(&Theme::get(), [t] { Theme::get().set(t); }, Qt::QueuedConnection);
    });
    auto* popBtn = new QPushButton("Popout");
    popBtn->setToolTip("A small always-on-top readout. Right-click it to change its skin.");
    connect(popBtn, &QPushButton::clicked, this, &MainWindow::togglePopout);
    themeRow->addWidget(pal_icon);
    themeRow->addWidget(themes, 1);
    themeRow->addWidget(popBtn);
    sv->addLayout(themeRow);
    h->addWidget(side);

    auto* main = new QWidget;
    auto* mv = new QVBoxLayout(main);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(26, 16, 26, 6);
    title_ = new QLabel;
    title_->setFont(Theme::get().heading(40));
    header->addWidget(title_);
    header->addStretch();
    mv->addLayout(header);

    stack_ = new QStackedWidget;
    summary_ = new SummaryPage;
    performance_ = new PerformancePage;
    processes_ = new ProcessesPage;
    history_ = new HistoryPage;
    settings_ = new SettingsPage(recorder_);
    log_ = new LogPage;
    weird_ = new WeirdPage;
    sysinfo_ = new SystemInfoPage;
    startup_ = new StartupPage;
    users_ = new UsersPage;
    services_ = new ServicesPage;
    connections_ = new ConnectionsPage;
    installed_ = new InstalledPage;
    updates_ = new UpdatesPage;
    disk_ = new DiskSpacePage;
    power_ = new PowerPage;
    for (QWidget* w : std::initializer_list<QWidget*>{summary_, performance_, processes_, history_, log_, weird_, sysinfo_, startup_, users_, services_,
                                                      connections_, installed_, updates_, disk_, power_, settings_})
        stack_->addWidget(w);
    connect(summary_, &SummaryPage::openHistory, this, [this] { openPage(3); });
    connect(history_, &HistoryPage::showProcess, this, [this](int pid) {
        openPage(2);
        processes_->selectPid(pid);
    });
    connect(weird_, &WeirdPage::showProcess, this, [this](int pid) {
        openPage(2);
        processes_->selectPid(pid);
    });
    mv->addWidget(stack_, 1);

    auto* bar = new QWidget;
    bar->setObjectName("statusbar");
    bar->setStyleSheet(QString("#statusbar { background:%1; border-top:1px solid %2; }").arg(P.sidebar.name(), P.border.name(QColor::HexArgb)));
    auto* bh = new QHBoxLayout(bar);
    bh->setContentsMargins(20, 7, 20, 7);
    auto* dot = new QLabel("●");
    dot->setStyleSheet(QString("color:%1; font-size:12px;").arg(P.good.name()));
    status_ = new QLabel("Starting");
    status_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
    status_->setTextFormat(Qt::RichText);
    eventDot_ = new QLabel;
    eventDot_->setStyleSheet(QString("color:%1; font-size:13px; font-weight:600;").arg(P.alert.name()));
    bh->addWidget(dot);
    bh->addWidget(status_, 1);
    bh->addWidget(eventDot_);
    mv->addWidget(bar);
    h->addWidget(main, 1);

    if (QWidget* old = centralWidget()) old->deleteLater();
    setCentralWidget(root);
    openPage(page_);
}

void MainWindow::openPage(int index) {
    page_ = index;
    if (nav_ && nav_->button(index)) nav_->button(index)->setChecked(true);
    if (stack_) stack_->setCurrentIndex(index);
    if (title_) title_->setText(kPages[index].label);
    refreshVisible();
}

void MainWindow::togglePopout() {
    if (!popout_) popout_ = new Popout;
    bool show = !popout_->isVisible();
    popout_->setVisible(show);
    QSettings().setValue("popout/open", show);
}

void MainWindow::showFromTray() {
    show();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (quitting_) {
        e->accept();
        return;
    }
    // Closing the window keeps recording; Quit lives in the tray menu.
    hide();
    e->ignore();
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    QMetaObject::invokeMethod(recorder_, [r = recorder_] { r->setWatching(true); }, Qt::QueuedConnection);
    refreshVisible();
}

void MainWindow::hideEvent(QHideEvent* e) {
    QMainWindow::hideEvent(e);
    QMetaObject::invokeMethod(recorder_, [r = recorder_] { r->setWatching(false); }, Qt::QueuedConnection);
}

void MainWindow::onSample(const as::SnapshotPtr& s) {
    Live::get().add(s);
    if (popout_) popout_->refresh();
    notifier_->onSample(s);
    bool hot = Notifier::danger(*s);
    if (hot != trayHot_) {
        trayHot_ = hot;
        updateTray();
    }
    if (!isVisible()) return;
    double cost = 0;
    for (const auto& [w, ms] : s->timings) cost += ms;
    // Separate facts, separated by space rather than punctuation.
    const QString gap = "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;";
    QString text = QString("Recording%1%2 processes%1%3 events today%1Last sample used %4 ms of CPU")
                       .arg(gap)
                       .arg(s->procs.size())
                       .arg(Live::get().eventsToday)
                       .arg(cost, 0, 'f', 1);
    if (!s->problems.empty()) text += gap + QString("%1 reader problem%2").arg(s->problems.size()).arg(s->problems.size() == 1 ? "" : "s");
    status_->setText(text);
    refreshVisible();
}

void MainWindow::onOpenEvent(const EventPtr& e, bool open) {
    Live& L = Live::get();
    bool changed = open != L.eventOpen;
    L.eventOpen = open;
    if (e || !open) L.openEvent = e;
    if (changed) {
        updateTray();
        emit L.eventChanged();
    }
    if (eventDot_) eventDot_->setText(open ? "●  Event being recorded" : "");
}

void MainWindow::refreshVisible() {
    if (!isVisible() || !stack_) return;
    switch (page_) {
    case 0: summary_->refresh(); break;
    case 1: performance_->refresh(); break;
    case 2: processes_->refresh(); break;
    case 3: history_->refresh(); break;
    case 4: log_->refresh(); break;
    case 5: weird_->refresh(); break;
    case 6: sysinfo_->refresh(); break;
    case 7: startup_->refresh(); break;
    case 8: users_->refresh(); break;
    case 9: services_->refresh(); break;
    case 10: connections_->refresh(); break;
    case 11: installed_->refresh(); break;
    case 12: updates_->refresh(); break;
    case 13: disk_->refresh(); break;
    case 14: power_->refresh(); break;
    default: break;
    }
}

void MainWindow::updateTray() {
    if (!tray_) return;
    bool alert = Live::get().eventOpen || trayHot_;
    tray_->setIcon(QIcon(Logo::trayPixmap(pal().phosphor ? pal().cpu : pal().gpu, alert)));
    if (trayHot_) tray_->setToolTip("ActivityScope: close to thermal shutdown");
    else if (alert) tray_->setToolTip("ActivityScope: recording an event");
    else if (tray_->toolTip().isEmpty() || tray_->toolTip().startsWith("ActivityScope: recording")) tray_->setToolTip("ActivityScope: recording");
}
