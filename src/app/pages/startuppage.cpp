#include "startuppage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "../format.h"
#include "../live.h"
#include "common.h"

namespace {

enum Role { KindRole = Qt::UserRole + 1, IdRole, OnRole };

struct Desktop {
    QString name, exec, icon;
    bool hidden = false, disabled = false, otherDesktop = false;
};

Desktop readDesktop(const QString& path) {
    Desktop d;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return d;
    bool inEntry = false;
    const QString desk = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    for (const auto& raw : QString::fromUtf8(f.readAll()).split('\n')) {
        QString line = raw.trimmed();
        if (line.startsWith('[')) {
            inEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inEntry) continue;
        QString k = line.section('=', 0, 0), v = line.section('=', 1);
        if (k == "Name") d.name = v;
        else if (k == "Exec") d.exec = v;
        else if (k == "Icon") d.icon = v;
        else if (k == "Hidden") d.hidden = v == "true";
        else if (k == "X-GNOME-Autostart-enabled" || k == "X-KDE-autostart-enabled") d.disabled = d.disabled || v == "false";
        else if (k == "OnlyShowIn") d.otherDesktop = !desk.isEmpty() && !v.split(';').contains(desk, Qt::CaseInsensitive);
        else if (k == "NotShowIn") d.otherDesktop = d.otherDesktop || (!desk.isEmpty() && v.split(';').contains(desk, Qt::CaseInsensitive));
    }
    return d;
}

QString userAutostart() { return QDir::homePath() + "/.config/autostart/"; }

}  // namespace

StartupPage::StartupPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    auto* bar = new QHBoxLayout;
    bar->addWidget(noteLabel("Programs that start when you log in: desktop autostart entries and your enabled background services."), 1);
    auto* filter = filterBox("Filter");
    bar->addWidget(filter);
    v->addLayout(bar);
    tree_ = styledTree({"Name", "Status", "Started by", "Runs", "Running now"});
    tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 260);
    tree_->header()->resizeSection(1, 170);
    tree_->header()->resizeSection(2, 210);
    tree_->header()->resizeSection(4, 130);
    v->addWidget(tree_, 1);
    auto* foot = new QHBoxLayout;
    msg_ = noteLabel();
    toggle_ = new QPushButton("Turn off");
    toggle_->setEnabled(false);
    foot->addWidget(msg_, 1);
    foot->addWidget(toggle_);
    v->addLayout(foot);
    connect(filter, &QLineEdit::textChanged, this, [this](const QString& f) { applyFilter(tree_, f); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        toggle_->setEnabled(it && it->data(0, KindRole).toString() != "other");
        if (it) toggle_->setText(it->data(0, OnRole).toBool() ? "Turn off" : "Turn on");
    });
    connect(toggle_, &QPushButton::clicked, this, &StartupPage::toggleSelected);
}

void StartupPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || (loaded_ > 0 && s->wall - loaded_ < 15)) return;
    loaded_ = s->wall;
    reload();
}

void StartupPage::reload() {
    QString keep = tree_->currentItem() ? tree_->currentItem()->data(0, IdRole).toString() : QString();
    tree_->setSortingEnabled(false);
    tree_->clear();
    const auto& s = Live::get().now();
    QSet<QString> running;
    if (s)
        for (const auto& p : s->procs) running.insert(qs(p.name).toLower());
    auto isRunning = [&](const QString& exec) {
        QString prog = exec.section(' ', 0, 0).section('/', -1).toLower();
        return !prog.isEmpty() && running.contains(prog);
    };

    // Desktop autostart: a file in ~/.config/autostart overrides the system one.
    QMap<QString, QString> files;
    for (const QString& dir : {QString("/etc/xdg/autostart/"), userAutostart()})
        for (const auto& f : QDir(dir).entryList({"*.desktop"}, QDir::Files)) files[f] = dir + f;
    for (auto it = files.begin(); it != files.end(); ++it) {
        Desktop d = readDesktop(*it);
        bool user = it->startsWith(userAutostart());
        bool on = !d.hidden && !d.disabled && !d.otherDesktop;
        auto* item = new NumItem(tree_);
        item->setText(0, d.name.isEmpty() ? it.key() : d.name);
        item->setText(1, d.otherDesktop ? "Not for this desktop" : on ? "On" : "Off");
        item->setText(2, user ? "Your autostart folder" : "System autostart");
        item->setText(3, d.exec);
        item->setText(4, isRunning(d.exec) ? "Yes" : "");
        item->setIcon(0, QIcon::fromTheme(d.icon, QIcon::fromTheme("application-x-executable")));
        item->setData(0, KindRole, d.otherDesktop ? "other" : "desktop");
        item->setData(0, IdRole, it.key());
        item->setData(0, OnRole, on);
        item->setForeground(1, on ? pal().good : pal().dim);
    }

    // Your systemd services that are enabled to start with the session.
    auto doc = QJsonDocument::fromJson(runQuick("systemctl", {"--user", "list-unit-files", "--type=service", "--state=enabled", "-o", "json", "--no-pager"}).toUtf8());
    for (const auto& v : doc.array()) {
        QString unit = v.toObject().value("unit_file").toString();
        if (unit.endsWith("@.service")) continue;
        auto* item = new NumItem(tree_);
        item->setText(0, unit.chopped(8));
        item->setText(1, "On");
        item->setText(2, "Your services (systemd)");
        item->setText(3, unit);
        bool active = runQuick("systemctl", {"--user", "is-active", unit}).trimmed() == "active";
        item->setText(4, active ? "Yes" : "");
        item->setIcon(0, iconPixmap("server", pal().dim, 18));
        item->setData(0, KindRole, "unit");
        item->setData(0, IdRole, unit);
        item->setData(0, OnRole, true);
        item->setForeground(1, pal().good);
    }
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (tree_->topLevelItem(i)->data(0, IdRole).toString() == keep) tree_->setCurrentItem(tree_->topLevelItem(i));
}

void StartupPage::toggleSelected() {
    auto* it = tree_->currentItem();
    if (!it) return;
    const QString kind = it->data(0, KindRole).toString(), id = it->data(0, IdRole).toString();
    const bool on = it->data(0, OnRole).toBool();
    if (kind == "unit") {
        int rc = QProcess::execute("systemctl", {"--user", on ? "disable" : "enable", id});
        msg_->setText(rc == 0 ? QString("%1 will %2 start at login.").arg(it->text(0), on ? "no longer" : "now")
                              : "systemctl refused the change.");
    } else {
        // Turning off writes a copy into your own autostart folder marked
        // Hidden, which is the standard way and leaves system files alone.
        QString mine = userAutostart() + id;
        QDir().mkpath(userAutostart());
        QString source = QFile::exists(mine) ? mine : "/etc/xdg/autostart/" + id;
        QFile in(source);
        QStringList lines;
        if (in.open(QIODevice::ReadOnly)) lines = QString::fromUtf8(in.readAll()).split('\n');
        in.close();
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString& l) {
                        return l.startsWith("Hidden=") || l.startsWith("X-GNOME-Autostart-enabled=");
                    }),
                    lines.end());
        int at = std::max(0, int(lines.indexOf("[Desktop Entry]")) + 1);
        lines.insert(at, on ? "Hidden=true" : "Hidden=false");
        QFile out(mine);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(lines.join('\n').toUtf8());
        msg_->setText(QString("%1 will %2 start at login (setting saved in %3).").arg(it->text(0), on ? "no longer" : "now", mine));
    }
    loaded_ = 0;
    reload();
}
