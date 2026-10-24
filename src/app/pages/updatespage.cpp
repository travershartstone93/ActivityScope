#include "updatespage.h"

#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QFile>
#include <QHBoxLayout>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "../format.h"
#include "common.h"
#include "logbook.h"

UpdatesPage::~UpdatesPage() {
    std::lock_guard<std::mutex> l(guard_->m);
    guard_->page = nullptr;
}

UpdatesPage::UpdatesPage(QWidget* parent) : QWidget(parent) {
    guard_ = std::make_shared<Guard>();
    guard_->page = this;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(10);
    auto* bar = new QHBoxLayout;
    status_ = noteLabel();
    checkBtn_ = new QPushButton("Check for updates");
    allBtn_ = new QPushButton("Update all");
    allBtn_->setEnabled(false);
    allBtn_->setToolTip("Opens a terminal that runs the full upgrade; you type your password there.");
    bar->addWidget(status_, 1);
    bar->addWidget(checkBtn_);
    bar->addWidget(allBtn_);
    v->addLayout(bar);
    holds_ = noteLabel();
    v->addWidget(holds_);
    progressBox_ = new QWidget;
    auto* pv = new QVBoxLayout(progressBox_);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(4);
    progressLabel_ = noteLabel();
    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    progress_->setFixedHeight(8);
    pv->addWidget(progressLabel_);
    pv->addWidget(progress_);
    progressBox_->hide();
    v->addWidget(progressBox_);
    alertBox_ = new QWidget;
    alerts_ = new QVBoxLayout(alertBox_);
    alerts_->setContentsMargins(0, 0, 0, 0);
    alerts_->setSpacing(8);
    v->addWidget(alertBox_);

    tree_ = styledTree({"Package", "Installed", "Available", "From", "Note", ""});
    tree_->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 260);
    tree_->header()->resizeSection(1, 170);
    tree_->header()->resizeSection(2, 170);
    tree_->header()->resizeSection(3, 110);
    tree_->header()->resizeSection(5, 190);
    tree_->setStyleSheet("QTreeView { font-size: 14px; } QTreeView::item { height: 36px; }");
    v->addWidget(tree_, 1);
    v->addWidget(noteLabel("Nothing is checked or installed unless you press a button. Updates run in a terminal so you can see "
                           "them and type your password; ActivityScope itself never gets administrator rights. Updating one "
                           "package on its own can leave it ahead of the libraries it was built with; Update all is the safe way."));

    connect(checkBtn_, &QPushButton::clicked, this, &UpdatesPage::check);
    connect(allBtn_, &QPushButton::clicked, this, [this] {
        bool aur = std::any_of(updates_.begin(), updates_.end(), [](const auto& u) { return u.aur; });
        runInTerminal("updating all packages", QString::fromStdString(as::update_all_command(held(), aur)));
    });

    double last = lastFullUpgrade();
    status_->setText(last > 0 ? QString("Last full upgrade %1 (%2 ago). Press Check for updates to see what is new.")
                                    .arg(QDateTime::fromSecsSinceEpoch(qint64(last)).toString("MMM d"), duration(QDateTime::currentSecsSinceEpoch() - last))
                              : QString("Press Check for updates to see what is new."));
    fill();
}

std::set<std::string> UpdatesPage::heldBySystem() const {
    QFile f("/etc/pacman.conf");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return as::pacman_ignored(f.readAll().toStdString());
}

std::set<std::string> UpdatesPage::held() const {
    std::set<std::string> h = heldBySystem();
    for (const auto& n : QSettings().value("held_packages").toStringList()) h.insert(n.toStdString());
    return h;
}

void UpdatesPage::setHeld(const std::string& name, bool hold) {
    QSettings st;
    QStringList l = st.value("held_packages").toStringList();
    QString n = QString::fromStdString(name);
    if (hold && !l.contains(n)) l << n;
    if (!hold) l.removeAll(n);
    st.setValue("held_packages", l);
    fill();
    analyse();  // the held set changed, so the rehearsal's verdict has too
}

double UpdatesPage::lastFullUpgrade() const {
    // Read the log backwards in chunks: the line wanted is almost always near
    // the end, and the file grows for years.
    QFile f("/var/log/pacman.log");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const qint64 chunk = 256 * 1024;
    qint64 pos = f.size();
    QByteArray carry;
    while (pos > 0) {
        qint64 start = qMax<qint64>(0, pos - chunk);
        f.seek(start);
        QByteArray buf = f.read(pos - start) + carry;
        int idx = buf.lastIndexOf("starting full system upgrade");
        if (idx >= 0) {
            int ls = buf.lastIndexOf('\n', idx);
            int le = buf.indexOf('\n', idx);
            QByteArray line = buf.mid(ls + 1, le < 0 ? -1 : le - ls - 1);
            return as::parse_pacman_time(line.toStdString());
        }
        carry = buf.left(200);  // a line split across the chunk edge is still found
        pos = start;
    }
    return 0;
}

void UpdatesPage::check() {
    checkBtn_->setEnabled(false);
    allBtn_->setEnabled(false);
    status_->setText("Checking the official repositories and the AUR...");
    setBusy(true, "Downloading the latest package lists (step 1 of 2)");
    updates_.clear();
    checkErrors_.clear();
    analysed_ = false;
    showAlerts();
    pending_ = 2;
    // checkupdates works from a private copy of the package lists, so the
    // system's own database is never touched (no partial upgrades).
    for (bool aur : {false, true}) {
        auto* p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p, aur](int code, QProcess::ExitStatus st) {
            auto list = as::parse_update_list(QString::fromUtf8(p->readAllStandardOutput()).toStdString(), aur);
            updates_.insert(updates_.end(), list.begin(), list.end());
            // checkupdates: 0 updates found, 2 none, 1 failed. paru -Qua: 0 or 1.
            const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
            if (st != QProcess::NormalExit || (!aur && code == 1) || (aur && code != 0 && !err.isEmpty()))
                checkErrors_ << QString(aur ? "the AUR check failed" : "the repository check failed") + (err.isEmpty() ? "" : " (" + err.section('\n', -1) + ")");
            p->deleteLater();
            if (--pending_ == 0) finishCheck();
        });
        connect(p, &QProcess::errorOccurred, this, [this, p, aur](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            checkErrors_ << (aur ? "paru is not installed, so AUR packages were not checked" : "checkupdates is not installed (package pacman-contrib)");
            p->deleteLater();
            if (--pending_ == 0) finishCheck();
        });
        if (aur) p->start("paru", {"-Qua"});
        else {
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("CHECKUPDATES_DB", checkDb());
            p->setProcessEnvironment(env);
            p->start("checkupdates", {});
        }
    }
}

void UpdatesPage::finishCheck() {
    checked_ = true;
    checkBtn_->setEnabled(true);
    std::sort(updates_.begin(), updates_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    fill();
    setBusy(false);
    analyse();
}

QString UpdatesPage::checkDb() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    return base + "/activityscope/checkup-db";
}

void UpdatesPage::analyse() {
    if (analysing_) return;
    if (std::none_of(updates_.begin(), updates_.end(), [](const auto& u) { return !u.aur; })) return;
    // A rehearsal of Update all on a scratch copy of the package database:
    // nothing on the system changes, and no administrator rights are needed.
    as::CheckInput in;
    in.sync_db = (checkDb() + "/sync").toStdString();
    in.scratch = (QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/upgrade-check").toStdString();
    in.held = held();
    in.updates = updates_;
    in.home = QDir::homePath().toStdString();
    in.progress = [g = guard_](int pct, const std::string& what) {
        std::lock_guard<std::mutex> l(g->m);
        if (!g->page) return;
        QMetaObject::invokeMethod(g->page, [p = g->page, pct, w = QString::fromStdString(what)] {
            if (p->analysing_) p->setBusy(true, "Looking for anything that would break the update (step 2 of 2): " + w, pct);
        }, Qt::QueuedConnection);
    };
    analysing_ = true;
    checkBtn_->setEnabled(false);
    setBusy(true, "Looking for anything that would break the update (step 2 of 2)", 0);
    showAlerts();
    auto* watch = new QFutureWatcher<as::CheckResult>(this);
    connect(watch, &QFutureWatcher<as::CheckResult>::finished, this, [this, watch] {
        result_ = watch->result();
        watch->deleteLater();
        analysing_ = false;
        analysed_ = true;
        checkBtn_->setEnabled(true);
        setBusy(false);
        showAlerts();
    });
    watch->setFuture(QtConcurrent::run([in] { return as::check_upgrade(in); }));
}

void UpdatesPage::showAlerts() {
    while (auto* item = alerts_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    auto line = [this](const QString& text, const QColor& c) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(QString("color:%1; font-size:14px;").arg(c.name()));
        alerts_->addWidget(l);
    };
    if (analysing_) return;
    if (!analysed_) return;
    const auto& r = result_;
    const bool fails = std::any_of(r.issues.begin(), r.issues.end(), [](const auto& i) { return i.severity == "fail"; });
    QString sizes = QString("Downloads %1, needs %2 more space, %3 free.")
                        .arg(QString::fromStdString(as::human_bytes(r.download_bytes)),
                             QString::fromStdString(as::human_bytes(std::max(0.0, r.extra_bytes))),
                             QString::fromStdString(as::human_bytes(r.free_bytes)));
    if (!fails && r.installs_cleanly)
        line(QString("Update all was rehearsed on a copy: %1 packages install cleanly. %2").arg(r.planned).arg(sizes), pal().good);

    for (const auto& is : r.issues) {
        const QColor c = is.severity == "fail" ? pal().alert : is.severity == "warn" ? pal().thermal : pal().dim;
        auto* card = new QFrame;
        card->setObjectName("alertCard");
        card->setStyleSheet(QString("#alertCard { background:%1; border:1px solid %2; border-left:4px solid %3; border-radius:6px; }")
                                .arg(pal().panel.name(), withAlpha(c, 90).name(QColor::HexArgb), c.name()));
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(14, 10, 14, 10);
        cv->setSpacing(6);
        auto* head = new QLabel(QString::fromStdString(is.title));
        head->setWordWrap(true);
        head->setStyleSheet(QString("color:%1; font-size:15px; font-weight:600;").arg(c.name()));
        cv->addWidget(head);
        auto* body = new QLabel(QString::fromStdString(is.explanation));
        body->setWordWrap(true);
        body->setStyleSheet(QString("color:%1; font-size:14px;").arg(pal().text.name()));
        cv->addWidget(body);
        QStringList facts;
        for (const auto& f : is.facts) facts << QString::fromStdString(f).toHtmlEscaped();
        for (const auto& fx : is.fixes)
            if (fx.removes.size() > fx.targets.size()) {
                QStringList rm;
                for (const auto& x : fx.removes) rm << QString::fromStdString(x);
                facts << "The removal takes out: " + rm.join(", ").toHtmlEscaped();
            }
        if (!facts.isEmpty()) {
            auto* f = new QLabel("&bull; " + facts.join("<br>&bull; "));
            f->setWordWrap(true);
            f->setTextFormat(Qt::RichText);
            f->setStyleSheet(QString("color:%1; font-size:13px;").arg(pal().dim.name()));
            cv->addWidget(f);
        }
        if (!is.raw.empty()) {
            auto* raw = new QLabel("pacman said: " + QString::fromStdString(is.raw));
            raw->setWordWrap(true);
            raw->setTextInteractionFlags(Qt::TextSelectableByMouse);
            raw->setStyleSheet(QString("color:%1; font-family:monospace; font-size:12px;").arg(pal().faint.name()));
            cv->addWidget(raw);
        }
        if (is.tested) {
            auto* t = new QLabel(QString("Tested: with this fix applied to a copy, Update all goes through (%1 packages).").arg(r.planned));
            t->setWordWrap(true);
            t->setStyleSheet(QString("color:%1; font-size:13px;").arg(pal().good.name()));
            cv->addWidget(t);
        }
        if (!is.fixes.empty()) {
            auto* row = new QHBoxLayout;
            row->addStretch(1);
            for (const auto& fx : is.fixes) {
                auto* b = new QPushButton(QString::fromStdString(fx.label));
                if (fx.kind == "remove")
                    b->setStyleSheet(QString("QPushButton { color:%1; border:1px solid %1; } QPushButton:hover { background:%2; }")
                                         .arg(c.name(), withAlpha(c, 40).name(QColor::HexArgb)));
                if (!fx.command.empty()) b->setToolTip("Opens a terminal that runs: " + QString::fromStdString(fx.command));
                if (fx.kind == "hold") {
                    b->setToolTip("Keep it at its current version; Update all will skip it.");
                    connect(b, &QPushButton::clicked, this, [this, fx] {
                        for (const auto& t : fx.targets) setHeld(t, true);
                    });
                } else {
                    connect(b, &QPushButton::clicked, this, [this, fx] {
                        QStringList t;
                        for (const auto& x : fx.targets) t << QString::fromStdString(x);
                        recheckOnClose_ = true;
                        runInTerminal(fx.kind == "remove" ? "removing " + t.join(", ") : "rebuilding " + t.join(", "),
                                      QString::fromStdString(fx.command));
                    });
                }
                row->addWidget(b);
            }
            cv->addLayout(row);
        }
        alerts_->addWidget(card);
    }
    if (fails && r.installs_cleanly && r.needed_fixes)
        line(QString("Once the fixes above are done, Update all goes through: %1 packages. %2").arg(r.planned).arg(sizes), pal().dim);
}

void UpdatesPage::fill() {
    const auto system = heldBySystem();
    std::set<std::string> all = system;
    for (const auto& n : QSettings().value("held_packages").toStringList()) all.insert(n.toStdString());
    QStringList heldNames;
    for (const auto& h : all) heldNames << QString::fromStdString(h) + (system.count(h) ? " (pacman.conf)" : " (ActivityScope)");
    holds_->setText(heldNames.isEmpty() ? QString("No packages are being kept at their current version.")
                                        : "Kept at their current version, never updated from here: " + heldNames.join(", ") + ".");
    tree_->clear();
    int restart = 0, updatable = 0;
    for (const auto& u : updates_) {
        auto* it = new QTreeWidgetItem(tree_);
        const QString name = QString::fromStdString(u.name);
        const bool isHeld = all.count(u.name) > 0;
        it->setText(0, name);
        it->setText(1, QString::fromStdString(u.installed));
        it->setText(2, QString::fromStdString(u.available));
        it->setText(3, u.aur ? "AUR" : "Official");
        QStringList notes;
        if (isHeld) notes << "kept at its current version";
        if (as::needs_restart(u.name)) {
            notes << "needs a restart";
            if (!isHeld) ++restart;
        }
        it->setText(4, notes.join(", "));
        if (isHeld) it->setForeground(4, pal().temp);
        else if (as::needs_restart(u.name)) it->setForeground(4, pal().thermal);
        it->setIcon(0, iconPixmap("package", isHeld ? pal().faint : pal().memory, 16));

        auto* cell = new QWidget;
        auto* h = new QHBoxLayout(cell);
        h->setContentsMargins(2, 2, 2, 2);
        h->setSpacing(6);
        if (isHeld) {
            if (system.count(u.name)) {
                auto* l = new QLabel("Held by pacman.conf");
                l->setStyleSheet(QString("color:%1;").arg(pal().dim.name()));
                h->addWidget(l);
            } else {
                auto* release = new QPushButton("Release");
                release->setToolTip("Allow this package to be updated again.");
                connect(release, &QPushButton::clicked, this, [this, n = u.name] { setHeld(n, false); });
                h->addWidget(release);
            }
        } else {
            ++updatable;
            if (u.aur) {
                // AUR packages build standalone, so a one-package update is
                // sound. A repo package is not: installing one against stale
                // lists is either a no-op or the partial upgrade Arch warns
                // about, so repo packages only update through Update all.
                auto* up = new QPushButton("Update");
                up->setToolTip(QString::fromStdString(as::update_one_command(u)));
                connect(up, &QPushButton::clicked, this, [this, u] { runInTerminal("updating " + QString::fromStdString(u.name), QString::fromStdString(as::update_one_command(u))); });
                h->addWidget(up);
            } else {
                auto* l = new QLabel("via Update all");
                l->setToolTip("Repo packages update together: one alone against unrefreshed lists would do nothing, or make a partial upgrade.");
                l->setStyleSheet(QString("color:%1;").arg(pal().dim.name()));
                h->addWidget(l);
            }
            auto* hold = new QPushButton("Hold");
            hold->setToolTip("Keep this package at its current version.");
            connect(hold, &QPushButton::clicked, this, [this, n = u.name] { setHeld(n, true); });
            h->addWidget(hold);
        }
        tree_->setItemWidget(it, 5, cell);
    }
    updatable_ = updatable;
    allBtn_->setEnabled(updatable > 0 && !analysing_ && pending_ == 0);
    const bool allGood = checked_ && checkErrors_.isEmpty() && updates_.empty();
    status_->setStyleSheet(allGood ? QString("color:%1; font-size:18px; font-weight:600;").arg(pal().good.name())
                                   : QString("color:%1; font-size:13px;").arg(pal().dim.name()));
    if (checked_ && !checkErrors_.isEmpty()) {
        status_->setText("The update check did not finish: " + checkErrors_.join("; ") + ". What is shown may be incomplete.");
    } else if (checked_) {
        status_->setText(updates_.empty() ? QString("Everything is up to date.")
                                          : QString("%1 update%2 available%3%4.")
                                                .arg(updatable)
                                                .arg(updatable == 1 ? "" : "s")
                                                .arg(restart ? QString(", %1 need%2 a restart afterwards").arg(restart).arg(restart == 1 ? "s" : "") : QString())
                                                .arg(int(updates_.size()) > updatable ? QString(", %1 held").arg(int(updates_.size()) - updatable) : QString()));
    }
}

void UpdatesPage::setBusy(bool busy, const QString& what, int percent) {
    // While checking, the update buttons wait: what they would run may be about to change.
    progressBox_->setVisible(busy);
    tree_->setEnabled(!busy);
    allBtn_->setEnabled(!busy && updatable_ > 0);
    if (!busy) {
        allBtn_->setToolTip("Opens a terminal that runs the full upgrade; you type your password there.");
        return;
    }
    allBtn_->setToolTip("Wait for the check to finish.");
    progressLabel_->setText(what);
    if (percent < 0) progress_->setRange(0, 0);  // no way to tell how far along: moving bar
    else {
        progress_->setRange(0, 100);
        progress_->setValue(percent);
    }
    progress_->setStyleSheet(QString("QProgressBar { background:%1; border:1px solid %2; border-radius:4px; }"
                                     "QProgressBar::chunk { background:%3; border-radius:3px; }")
                                 .arg(pal().panel.name(), pal().border.name(), pal().gpu.name()));
}

void UpdatesPage::runInTerminal(const QString& what, const QString& command) {
    // A visible terminal: you see every step and type your own password.
    // A hindsight snapshot first, when the tool is installed: if the update
    // changes behaviour, the question "what changed?" has a before picture.
    const QString script = QString("command -v hindsight >/dev/null && hindsight snap --reason 'before %1'; "
                                   "echo 'ActivityScope: %1'; echo '$ %2'; echo; %2; echo; read -rp 'Finished. Press Enter to close this window.'")
                               .arg(what, command);
    QString term = QStandardPaths::findExecutable("gnome-terminal");
    QStringList args = {"--wait", "--title", "ActivityScope update", "--", "bash", "-c", script};
    if (term.isEmpty()) {
        term = QStandardPaths::findExecutable("konsole");
        args = {"-e", "bash", "-c", script};
    }
    if (term.isEmpty()) {
        term = QStandardPaths::findExecutable("kitty");
        args = {"bash", "-c", script};
    }
    if (term.isEmpty()) {
        term = QStandardPaths::findExecutable("alacritty");
        args = {"-e", "bash", "-c", script};
    }
    if (term.isEmpty()) {
        term = QStandardPaths::findExecutable("foot");
        args = {"bash", "-c", script};
    }
    if (term.isEmpty()) {
        recheckOnClose_ = false;
        status_->setText("No terminal program was found (gnome-terminal, konsole, kitty, alacritty or foot), so nothing was run.");
        return;
    }
    auto* p = new QProcess(this);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        recheckOnClose_ = false;
        status_->setText("The terminal could not start, so nothing was run.");
        p->deleteLater();
    });
    connect(p, &QProcess::finished, this, [this, p] {
        p->deleteLater();
        if (recheckOnClose_) {
            recheckOnClose_ = false;
            status_->setText("The terminal window closed. Checking again whether the problem is gone...");
            analyse();
            return;
        }
        status_->setText("The terminal window closed. Press Check for updates to refresh the list.");
    });
    p->start(term, args);
    status_->setText("Running in the terminal window...");
}
