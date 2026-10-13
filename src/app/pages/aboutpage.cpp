#include "aboutpage.h"

#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../theme.h"
#include "../widgets/card.h"
#include "../widgets/logo.h"
#include "common.h"
#include "store.h"

namespace {

QLabel* para(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color:%1; font-size:14px;").arg(pal().text.name()));
    return l;
}

void featureRow(QVBoxLayout* into, const char* icon, const QString& name, const QString& what) {
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    auto* ic = new QLabel;
    ic->setPixmap(iconPixmap(icon, pal().gpu, 18));
    ic->setFixedWidth(22);
    auto* text = new QLabel(QString("<b>%1</b>&nbsp;&nbsp;<span style='color:%2'>%3</span>")
                                .arg(name, pal().dim.name(), what));
    text->setWordWrap(true);
    text->setStyleSheet("font-size:14px;");
    row->addWidget(ic, 0, Qt::AlignTop);
    row->addWidget(text, 1);
    into->addLayout(row);
}

}  // namespace

AboutPage::AboutPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 0, 22, 14);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* grid = new QGridLayout(inner);
    grid->setContentsMargins(0, 0, 8, 0);
    grid->setSpacing(14);
    auto* left = new QVBoxLayout;
    auto* right = new QVBoxLayout;
    left->setSpacing(14);
    right->setSpacing(14);

    auto* head = new Card("activity", "ActivityScope " AS_VERSION);
    head->body()->addWidget(para(
        "A task manager that remembers. When something drags this machine down and then ends, every other tool has "
        "already forgotten; ActivityScope records what happened and who caused it, including programs that have "
        "since exited, so the question \"what just happened?\" always has an answer."));
    left->addWidget(head);

    auto* rec = new Card("server", "What it records, every second");
    rec->body()->addWidget(para(
        "CPU (whole, per core and per program), memory and swap, disks, network, GPU, temperatures, fans and power. "
        "When any trigger holds for a few seconds an event opens, carrying the minute before it, the per-second "
        "graphs, and the programs responsible, ranked by their share. Short-lived programs that die between "
        "glances are still caught and attributed to whoever started them. Every event is judged when it closes: "
        "work you started yourself is routine and drawn quietly; red is kept for shutdown danger, swap trouble, a "
        "dead fan, or a background program as the main cause. One day of full detail, a month of events, three "
        "months of log are kept, in a database any tool can read."));
    left->addWidget(rec);

    auto* guard = new Card("thermometer", "The shutdown guard");
    guard->body()->addWidget(para(
        "The kernel turns this machine off when its critical thermal zone reaches the limit; ActivityScope reads "
        "that limit from the system and watches the distance to it. It can tell a dead fan from a resting one, "
        "warns before a shutdown with the name of the program responsible and a Pause button, and writes every "
        "second to disk near the limit so even a sudden power-off leaves a record of its last moments."));
    left->addWidget(guard);

    auto* kit = new Card("search", "The investigation kit");
    kit->body()->addWidget(para(
        "The Log notes things worth remembering during an investigation: USB devices plugged in and removed, "
        "drives formatted, boards flashed, services failing, crashes, logins, network firsts, new startup entries, "
        "and the first run of any program from outside the package system. The Weird processes page runs the "
        "bundled weird scanner, which judges whether what is running and installed can be trusted from the "
        "distribution's own signed package data. A system-wide installation of weird is preferred when present; "
        "otherwise the copy shipped with this program is used, so nothing else needs installing."));
    kit->body()->addWidget(para(
        "The hindsight command, installed alongside, is the config time machine: a daily snapshot of /etc and your "
        "config files into a private local repository (plus one before every update run from here), and one question, "
        "hindsight \"2 days ago\", that interleaves events, config changes and package history into a single story."));
    kit->body()->addWidget(para(
        "The whole history is one SQLite file, made to be read by people and their tools alike: " +
        QString::fromStdString(as::default_db_path())));
    left->addWidget(kit);

    auto* pages = new Card("layout-dashboard", "The pages");
    auto* pb = pages->body();
    featureRow(pb, "layout-dashboard", "Summary", "the machine at a glance, with an event banner when one is open");
    featureRow(pb, "activity", "Performance", "every resource with its history, back through the last day");
    featureRow(pb, "list-tree", "Processes", "sort, inspect, pause and end processes, as a list or a tree");
    featureRow(pb, "server", "History", "the timeline of events and their culprits, in plain sentences");
    featureRow(pb, "scroll-text", "Log", "the investigation log of everything worth remembering");
    featureRow(pb, "search", "Weird processes", "the bundled weird scanner's judgements, in its own words");
    featureRow(pb, "info", "System info", "hardware, firmware and operating system facts");
    featureRow(pb, "power", "Startup apps", "what starts at login, and a watch on startup locations");
    featureRow(pb, "users", "Users", "who is logged in and what they are running");
    featureRow(pb, "cog", "Services", "systemd services, with start, stop and restart for your own");
    featureRow(pb, "cable", "Connections", "every open connection and the program behind it");
    featureRow(pb, "package", "Installed apps", "everything installed, from packages to AUR builds");
    featureRow(pb, "download", "Updates", "updates on request only, rehearsed on a copy before they run");
    featureRow(pb, "chart-pie", "Disk space", "what is using the disks");
    featureRow(pb, "gauge", "Power & clocks", "frequencies, governors, battery and energy use");
    featureRow(pb, "settings", "Settings", "themes, triggers with presets, retention and the guard");
    right->addWidget(pages);

    auto* rules = new Card("zap", "The rules it lives by");
    rules->body()->addWidget(para(
        "Never root: the program runs as you, always; anything needing your password happens in a terminal you can "
        "read. Nothing phones home: no network checks, no telemetry, and your history never leaves this machine. "
        "Nothing happens on its own: checking, updating and scanning wait for a button. Plain words: it explains "
        "in sentences, and where it cannot measure something it says so instead of showing a zero."));
    right->addWidget(rules);

    auto* credits = new Card("package", "Built with");
    credits->body()->addWidget(para(
        "Qt and SQLite. Oxanium type (SIL Open Font License), Lucide icons (ISC). The bundled weird scanner is the "
        "npm package thatsweird (MIT, same author). ActivityScope itself is MIT licensed; the look is inspired by "
        "Dave Plummer's Task Manager, rebuilt from scratch."));
    right->addWidget(credits);

    left->addStretch();
    right->addStretch();
    grid->addLayout(left, 0, 0);
    grid->addLayout(right, 0, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    scroll->setWidget(inner);
    outer->addWidget(scroll);
}
