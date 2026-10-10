#include "installedpage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "../format.h"
#include "../live.h"
#include "common.h"

namespace {

QList<AppEntry> loadApps() {
    // Which package owns each file, from pacman's local database.
    QHash<QString, int> owner;
    struct Pkg { QString name, version; double installed = 0, size = 0; };
    QList<Pkg> pkgs;
    const QString db = "/var/lib/pacman/local/";
    for (const auto& dir : QDir(db).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Pkg p;
        QFile desc(db + dir + "/desc");
        if (desc.open(QIODevice::ReadOnly)) {
            QString key;
            for (const auto& line : QString::fromUtf8(desc.readAll()).split('\n')) {
                if (line.startsWith('%')) key = line;
                else if (!line.isEmpty() && key == "%NAME%") p.name = line;
                else if (!line.isEmpty() && key == "%VERSION%") p.version = line;
                else if (!line.isEmpty() && key == "%INSTALLDATE%") p.installed = line.toDouble();
                else if (!line.isEmpty() && key == "%SIZE%") p.size = line.toDouble();
            }
        }
        QFile files(db + dir + "/files");
        if (files.open(QIODevice::ReadOnly)) {
            int idx = int(pkgs.size());
            for (const auto& line : QString::fromUtf8(files.readAll()).split('\n'))
                if (line.endsWith(".desktop") && line.contains("share/applications/")) owner.insert("/" + line, idx);
        }
        pkgs.push_back(p);
    }
    QList<AppEntry> apps;
    QSet<QString> seen;
    const QString home = QDir::homePath();
    struct Dir { QString path, source; };
    for (const Dir& d : {Dir{home + "/.local/share/applications/", "Added by you"},
                         Dir{home + "/.local/share/flatpak/exports/share/applications/", "Flatpak"},
                         Dir{"/var/lib/flatpak/exports/share/applications/", "Flatpak"},
                         Dir{"/var/lib/snapd/desktop/applications/", "Snap"}, Dir{"/usr/share/applications/", "Package"}}) {
        for (const auto& f : QDir(d.path).entryList({"*.desktop"}, QDir::Files)) {
            if (seen.contains(f)) continue;  // a user copy overrides the system one
            seen.insert(f);
            AppEntry a;
            a.file = d.path + f;
            a.source = d.source;
            QFile in(a.file);
            if (!in.open(QIODevice::ReadOnly)) continue;
            bool entry = false, hidden = false, isApp = true;
            for (const auto& raw : QString::fromUtf8(in.readAll()).split('\n')) {
                QString line = raw.trimmed();
                if (line.startsWith('[')) {
                    entry = line == "[Desktop Entry]";
                    continue;
                }
                if (!entry) continue;
                QString k = line.section('=', 0, 0), v = line.section('=', 1);
                if (k == "Name") a.name = v;
                else if (k == "Comment") a.comment = v;
                else if (k == "Exec") a.exec = v;
                else if (k == "Icon") a.icon = v;
                else if (k == "NoDisplay" || k == "Hidden") hidden = hidden || v == "true";
                else if (k == "Type") isApp = v == "Application";
            }
            if (hidden || !isApp || a.name.isEmpty()) continue;
            auto o = owner.find(a.file);
            if (o != owner.end()) {
                const Pkg& p = pkgs[*o];
                a.package = p.name;
                a.version = p.version;
                a.installed = p.installed;
                a.size = p.size;
            }
            apps.push_back(a);
        }
    }
    return apps;
}

}  // namespace

InstalledPage::InstalledPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    auto* bar = new QHBoxLayout;
    count_ = noteLabel("Reading the app menu and the package database...");
    filter_ = filterBox("Filter by name, description or package");
    bar->addWidget(count_, 1);
    bar->addWidget(filter_);
    v->addLayout(bar);
    tree_ = styledTree({"App", "What it is", "Package", "Version", "Installed", "Size", "Running"});
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 230);
    tree_->header()->resizeSection(2, 170);
    tree_->header()->resizeSection(3, 130);
    tree_->header()->resizeSection(4, 120);
    tree_->header()->resizeSection(5, 100);
    tree_->header()->resizeSection(6, 90);
    tree_->setIconSize(QSize(22, 22));
    v->addWidget(tree_, 1);
    v->addWidget(noteLabel("Removing software needs administrator rights, so ActivityScope only lists it. "
                           "Size is the package's installed size."));
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) { applyFilter(tree_, f); });
    connect(&watcher_, &QFutureWatcher<QList<AppEntry>>::finished, this, [this] { fill(watcher_.result()); });
}

void InstalledPage::refresh() {
    if (started_) return;
    started_ = true;
    watcher_.setFuture(QtConcurrent::run(loadApps));
}

void InstalledPage::fill(const QList<AppEntry>& apps) {
    QSet<QString> running;
    if (const auto& s = Live::get().now())
        for (const auto& p : s->procs) running.insert(qs(p.name).toLower());
    tree_->setSortingEnabled(false);
    tree_->clear();
    for (const auto& a : apps) {
        auto* it = new NumItem(tree_);
        it->setText(0, a.name);
        it->setText(1, a.comment);
        it->setText(2, a.package.isEmpty() ? a.source : a.package);
        it->setText(3, a.version);
        if (a.installed > 0) {
            it->setText(4, QDateTime::fromSecsSinceEpoch(qint64(a.installed)).toString("MMM d, yyyy"));
            it->setData(4, Qt::UserRole, a.installed);
        }
        if (a.size > 0) {
            it->setText(5, bytes(a.size));
            it->setData(5, Qt::UserRole, a.size);
            it->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        }
        QString prog = a.exec.section(' ', 0, 0).section('/', -1).toLower();
        bool on = !prog.isEmpty() && running.contains(prog);
        it->setText(6, on ? "Yes" : "");
        it->setForeground(6, pal().good);
        it->setIcon(0, QIcon::fromTheme(a.icon, QIcon::fromTheme("application-x-executable")));
        it->setToolTip(0, a.file);
    }
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    count_->setText(QString("%1 apps in the menu.").arg(apps.size()));
}
