#include "userspage.h"

#include <QVBoxLayout>
#include <map>

#include "../format.h"
#include "../live.h"
#include "common.h"

UsersPage::UsersPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->addWidget(noteLabel("Every account with programs running, and its login sessions. System accounts run background services."));
    tree_ = styledTree({"Account", "Sessions", "Processes", "CPU", "Memory", "Threads"});
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 220);
    for (int c = 2; c < 6; ++c) tree_->header()->resizeSection(c, 130);
    tree_->sortByColumn(3, Qt::DescendingOrder);
    v->addWidget(tree_, 1);
}

void UsersPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || (loaded_ > 0 && s->wall - loaded_ < 2)) return;
    loaded_ = s->wall;
    struct Row { int procs = 0, threads = 0; double cpu = 0; uint64_t mem = 0; QStringList sessions; };
    std::map<QString, Row> rows;
    for (const auto& p : s->procs) {
        if (p.kernel_thread) continue;
        Row& r = rows[qs(p.user)];
        ++r.procs;
        r.threads += p.threads;
        r.cpu += p.cpu;
        r.mem += p.rss;
    }
    // loginctl prints: SESSION UID USER SEAT LEADER CLASS TTY IDLE SINCE
    for (const auto& line : runQuick("loginctl", {"list-sessions", "--no-legend", "--no-pager"}).split('\n')) {
        auto f = line.simplified().split(' ');
        if (f.size() < 7) continue;
        QString cls = f[5], tty = f[6] == "-" ? f[3] : f[6];
        QString what = cls == "user" ? QString("logged in on %1").arg(tty) : cls == "manager" ? QString("background manager") : cls;
        rows[f[2]].sessions << QString("#%1 %2").arg(f[0], what);
    }
    QString keep = tree_->currentItem() ? tree_->currentItem()->text(0) : QString();
    tree_->setSortingEnabled(false);
    tree_->clear();
    for (const auto& [user, r] : rows) {
        auto* it = new NumItem(tree_);
        it->setText(0, user);
        it->setText(1, r.sessions.isEmpty() ? "" : r.sessions.join(",  "));
        it->setText(2, QString::number(r.procs));
        it->setData(2, Qt::UserRole, r.procs);
        it->setText(3, pct(r.cpu));
        it->setData(3, Qt::UserRole, r.cpu);
        it->setText(4, bytes(double(r.mem)));
        it->setData(4, Qt::UserRole, double(r.mem));
        it->setText(5, QString::number(r.threads));
        it->setData(5, Qt::UserRole, r.threads);
        for (int c = 2; c < 6; ++c) it->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        it->setIcon(0, iconPixmap("users", r.sessions.isEmpty() ? pal().dim : pal().gpu, 18));
        if (user == keep) tree_->setCurrentItem(it);
    }
    tree_->setSortingEnabled(true);
}
