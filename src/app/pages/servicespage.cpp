#include "servicespage.h"

#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <map>

#include "../format.h"
#include "../live.h"
#include "common.h"

ServicesPage::ServicesPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    auto* bar = new QHBoxLayout;
    scope_ = new QComboBox;
    scope_->addItems({"Your services", "System services"});
    filter_ = filterBox("Filter by name or description");
    bar->addWidget(noteLabel("Background programs managed by systemd, with their memory and CPU right now."), 1);
    bar->addWidget(scope_);
    bar->addWidget(filter_);
    v->addLayout(bar);
    tree_ = styledTree({"Service", "Description", "State", "CPU", "Memory"});
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 300);
    tree_->header()->resizeSection(2, 160);
    tree_->header()->resizeSection(3, 100);
    tree_->header()->resizeSection(4, 120);
    v->addWidget(tree_, 1);
    auto* foot = new QHBoxLayout;
    msg_ = noteLabel();
    start_ = new QPushButton("Start");
    stop_ = new QPushButton("Stop");
    stop_->setObjectName("danger");
    restart_ = new QPushButton("Restart");
    foot->addWidget(msg_, 1);
    foot->addWidget(start_);
    foot->addWidget(restart_);
    foot->addWidget(stop_);
    v->addLayout(foot);
    auto sync = [this] {
        bool mine = scope_->currentIndex() == 0;
        auto* it = tree_->currentItem();
        for (auto* b : {start_, stop_, restart_}) b->setEnabled(mine && it);
        if (!mine) msg_->setText("System services can only be changed with administrator rights, which ActivityScope never uses.");
    };
    connect(scope_, &QComboBox::currentIndexChanged, this, [this, sync] {
        msg_->clear();
        loaded_ = 0;
        reload();
        sync();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) { applyFilter(tree_, f); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, sync);
    connect(start_, &QPushButton::clicked, this, [this] { act("start"); });
    connect(stop_, &QPushButton::clicked, this, [this] { act("stop"); });
    connect(restart_, &QPushButton::clicked, this, [this] { act("restart"); });
    sync();
}

void ServicesPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || (loaded_ > 0 && s->wall - loaded_ < 5)) return;
    loaded_ = s->wall;
    reload();
}

void ServicesPage::reload() {
    const bool mine = scope_->currentIndex() == 0;
    QStringList args = {"list-units", "--type=service", "--all", "-o", "json", "--no-pager"};
    if (mine) args.prepend("--user");
    auto doc = QJsonDocument::fromJson(runQuick("systemctl", args).toUtf8());
    // CPU comes from the recorder's per-unit accounting; memory from cgroups.
    std::map<QString, std::pair<double, QString>> units;
    if (const auto& s = Live::get().now())
        for (const auto& u : s->units) units[qs(u.name)] = {u.cpu, qs(u.path)};
    QString keep = tree_->currentItem() ? tree_->currentItem()->text(0) : QString();
    tree_->setSortingEnabled(false);
    tree_->clear();
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        QString unit = o.value("unit").toString();
        QString active = o.value("active").toString(), sub = o.value("sub").toString();
        if (o.value("load").toString() == "not-found") continue;
        auto* it = new NumItem(tree_);
        it->setText(0, unit);
        it->setText(1, o.value("description").toString());
        it->setText(2, active == sub ? active : active + " (" + sub + ")");
        it->setForeground(2, active == "active" ? pal().good : active == "failed" ? pal().alert : pal().dim);
        auto u = units.find(unit);
        if (u != units.end()) {
            it->setText(3, pct(u->second.first));
            it->setData(3, Qt::UserRole, u->second.first);
            QFile f("/sys/fs/cgroup" + u->second.second + "/memory.current");
            if (f.open(QIODevice::ReadOnly)) {
                double b = QString::fromUtf8(f.readAll()).trimmed().toDouble();
                it->setText(4, bytes(b));
                it->setData(4, Qt::UserRole, b);
            }
        }
        it->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        it->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        it->setIcon(0, iconPixmap("server", active == "active" ? pal().good : pal().faint, 18));
        if (unit == keep) tree_->setCurrentItem(it);
    }
    tree_->setSortingEnabled(true);
    applyFilter(tree_, filter_->text());
}

void ServicesPage::act(const QString& verb) {
    auto* it = tree_->currentItem();
    if (!it || scope_->currentIndex() != 0) return;
    QProcess p;
    p.start("systemctl", {"--user", verb, it->text(0)});
    p.waitForFinished(15000);
    QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    msg_->setText(p.exitCode() == 0 ? QString("%1: %2 done.").arg(it->text(0), verb) : err);
    loaded_ = 0;
    reload();
}
