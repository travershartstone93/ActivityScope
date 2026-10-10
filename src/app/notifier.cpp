#include "notifier.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QSettings>
#include <unistd.h>

#include "format.h"
#include "live.h"

namespace {

const char* kService = "org.freedesktop.Notifications";
const char* kPath = "/org/freedesktop/Notifications";

}  // namespace

Notifier::Notifier(QObject* parent) : QObject(parent) {
    auto bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, "ActionInvoked", this, SLOT(onAction(uint, QString)));
    bus.connect(kService, kPath, kService, "NotificationClosed", this, SLOT(onClosed(uint, uint)));
}

bool Notifier::danger(const as::Snapshot& s) {
    const auto& g = s.guard;
    if (!g.margin.ok()) return false;
    // Settings are re-read every 10 s rather than on every sample.
    static double warnMargin = 5, warnSeconds = 20, readAt = -1e9;
    if (s.mono - readAt >= 10) {
        QSettings st;
        warnMargin = st.value("warn_margin", 5).toDouble();
        warnSeconds = st.value("warn_seconds", 20).toDouble();
        readAt = s.mono;
    }
    return g.margin.get() <= warnMargin || (g.seconds_left.ok() && g.seconds_left.get() <= warnSeconds && g.margin.get() <= 15);
}

const as::ProcessInfo* Notifier::heatCulprit(const as::Snapshot& s, bool* pausable) {
    const as::ProcessInfo* best = nullptr;
    const as::ProcessInfo* mine = nullptr;
    const int uid = int(getuid());
    for (const auto& p : s.procs) {
        if (p.kernel_thread || p.self || p.state == "T") continue;
        // Own CPU only: a shell whose finished children used CPU is not
        // heating the machine now, and pausing it would not help.
        if (!best || p.cpu > best->cpu) best = &p;
        if (p.uid == uid && (!mine || p.cpu > mine->cpu)) mine = &p;
    }
    // Prefer something the user can pause, unless another user's process
    // is clearly the bigger cause.
    if (mine && best && best != mine && best->cpu > 2 * mine->cpu + 10) {
        if (pausable) *pausable = false;
        return best;
    }
    if (pausable) *pausable = mine != nullptr;
    return mine ? mine : best;
}

uint Notifier::notify(const QString& title, const QString& body, const QStringList& actions, bool critical, uint replaces) {
    QDBusInterface n(kService, kPath, kService, QDBusConnection::sessionBus());
    QVariantMap hints;
    hints["urgency"] = QVariant::fromValue<uchar>(critical ? 2 : 1);
    hints["desktop-entry"] = "activityscope";
    QDBusReply<uint> r = n.call("Notify", "ActivityScope", replaces, "activityscope", title, body, actions, hints, critical ? 0 : 15000);
    return r.isValid() ? r.value() : 0;
}

void Notifier::onSample(const as::SnapshotPtr& sp) {
    const as::Snapshot& s = *sp;
    const auto& g = s.guard;
    static bool enabled = true;
    static double enabledAt = -1e9;
    if (s.mono - enabledAt >= 10) {
        enabled = QSettings().value("warn_enabled", true).toBool();
        enabledAt = s.mono;
    }
    if (!enabled || !g.margin.ok()) return;
    const double now = s.wall;
    const double margin = g.margin.get();

    const bool hot = danger(s);
    dangerStreak_ = hot ? dangerStreak_ + 1 : 0;
    calmStreak_ = hot ? 0 : calmStreak_ + 1;
    if (hot && dangerStreak_ >= 2) {
        // Repeat at most once a minute, unless it is getting worse quickly.
        bool due = !warning_ || now - lastSent_ >= 60 || margin <= lastMargin_ - 2;
        warning_ = true;
        if (!due) return;
        lastSent_ = now;
        lastMargin_ = margin;
        bool pausable = false;
        const as::ProcessInfo* c = heatCulprit(s, &pausable);
        QString body = QString("%1 is at %2 °C; the system shuts itself down at %3 °C.")
                           .arg(qs(g.zone), QString::number(g.temp.get(), 'f', 0), QString::number(g.limit.get(), 'f', 0));
        if (g.seconds_left.ok()) body += QString(" At this rate that is about %1 seconds away.").arg(int(g.seconds_left.get()));
        QStringList actions;
        if (c) {
            body += QString(" %1 is using %2 of the CPU.").arg(qs(c->name), pct(c->cpu, 0));
            if (pausable) actions << "pause" << "Pause " + qs(c->name);
        }
        if (!g.dead_fans.empty()) body += QString(" The %1 is not spinning.").arg(qs(g.dead_fans.front()));
        actions << "open" << "Open ActivityScope";
        warningId_ = notify("Close to thermal shutdown", body, actions, true, warningId_);
        if (c && pausable) targets_[warningId_] = {c->key, qs(c->name)};
    } else if (warning_ && calmStreak_ >= 5) {
        // The danger has passed: take the warning off the screen.
        warning_ = false;
        lastMargin_ = 1e9;
        if (warningId_) {
            QDBusInterface(kService, kPath, kService, QDBusConnection::sessionBus()).call("CloseNotification", warningId_);
            targets_.remove(warningId_);
            warningId_ = 0;
        }
        const auto& paused = Live::get().paused;
        if (!paused.empty()) {
            const auto& p = paused.back();
            uint id = notify("Cooled down", QString("%1 is back to %2 °C. %3 is still paused.")
                                                .arg(qs(g.zone), QString::number(g.temp.get(), 'f', 0), p.name),
                             {"resume", "Resume " + p.name, "open", "Open ActivityScope"}, false);
            targets_[id] = {p.key, p.name};
        }
    }
}

void Notifier::onAction(uint id, const QString& action) {
    if (action == "open") {
        emit openRequested();
        return;
    }
    auto it = targets_.find(id);
    if (it == targets_.end()) return;
    Target t = *it;
    QString err;
    if (action == "pause") {
        if (Live::get().pause(t.key, t.name, &err)) {
            uint nid = notify("Paused " + t.name,
                              QString("%1 (PID %2) is paused to let the machine cool. It stays paused until you resume it, "
                                      "here or from ActivityScope.").arg(t.name).arg(t.key.pid),
                              {"resume", "Resume " + t.name, "open", "Open ActivityScope"}, false);
            targets_[nid] = t;
        } else {
            notify("Could not pause " + t.name, err, {"open", "Open ActivityScope"}, false);
        }
    } else if (action == "resume") {
        Live::get().resume(t.key, &err);
        notify("Resumed " + t.name, err.isEmpty() ? t.name + " is running again." : err, {}, false);
    }
}

void Notifier::onClosed(uint id, uint) {
    if (id == warningId_) warningId_ = 0;
}
