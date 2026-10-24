#include "recorder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <map>

#include "conkyfeed.h"
#include "guard.h"

Recorder::~Recorder() {
    if (!store_) return;
    store_->add_procs(pendingProcs_);  // up to 10 s of buffered records
    pendingProcs_.clear();
    store_->flush();
}

void Recorder::start() {
    sampler_ = std::make_unique<as::Sampler>();
    store_ = std::make_unique<as::Store>(as::default_db_path());
    if (!store_->ok()) emit storeError(QString::fromStdString(store_->error()));
    last_db_sample_ = store_->last_sample_time();
    feed_dir_ = as::runtime_dir();
    checkPastBoots();
    logwatch_ = std::make_unique<as::LogWatch>(store_.get());
    startup_ = std::make_unique<as::StartupWatch>();
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &Recorder::tick);
    timer_->start(1000);
    tick();
}

void Recorder::setInterval(int ms) {
    if (timer_) timer_->setInterval(std::max(250, ms));
}

void Recorder::tick() {
    as::SnapshotPtr s = sampler_->sample();
    store_->add_sample(*s);
    for (const auto& e : engine_.feed(s)) {
        int64_t id = store_->add_event(e);
        emit eventClosed(id, QString::fromStdString(e.summary));
    }
    if (auto m = engine_.take_minute()) store_->add_minute(*m);
    as::write_conky_feed(*s, feed_dir_);
    try {
    if (logwatch_) {
        auto entries = logwatch_->poll(*s);
        auto changes = startup_->poll(s->wall);
        entries.insert(entries.end(), changes.begin(), changes.end());
        auto started = proclog_.feed(*s);
        if (!reconciled_) {
            // First pass of this run: anything the database still thinks is
            // running, but is not alive now, ended while nothing recorded.
            reconciled_ = true;
            store_->close_stale_procs(started, last_db_sample_);
        }
        pendingProcs_.insert(pendingProcs_.end(), started.begin(), started.end());
        auto conns = netlog_.feed(*s);
        if (!conns.empty()) {
            double up = 0;
            for (const auto& n : s->nets)
                if (n.kind != "Loopback" && n.kind != "Virtual") up += n.tx_bps;
            peers_.allowed = [this](const std::string& p) { return allowedSharers_.count(p) > 0; };
            for (const auto& a : peers_.feed(conns, up, s->wall)) {
                // The record's PID was captured when the pair was first seen
                // and may be stale or recycled; only offer Quit on a live
                // process that really is this program.
                int pid = 0;
                uint64_t ticks = 0;
                for (const auto& p : s->procs)
                    if (p.key.pid == a.pid && p.name == a.program) {
                        pid = p.key.pid;
                        ticks = p.key.start_ticks;
                    }
                if (!pid)
                    for (const auto& p : s->procs)
                        if (p.name == a.program) {
                            pid = p.key.pid;
                            ticks = p.key.start_ticks;
                            break;
                        }
                entries.push_back({s->wall, "network",
                                   a.program + " is sharing with " + std::to_string(a.peers) + " different computers in the background",
                                   "Program: " + a.exe + "\nPID: " + std::to_string(pid) + "\nThe machine was uploading about " +
                                       as::human_bytes(a.upload_bps) + "/s at the time."});
                emit sharing(QString::fromStdString(a.program), pid, ticks, a.peers, a.upload_bps);
            }
        }
        for (const auto& c : store_->add_connections(conns)) {
            // A first connection is worth a log line when the program is not
            // part of the system (downloads, builds, scripts).
            if (c.exe.empty() || c.exe.rfind("/usr/", 0) == 0 || c.exe.rfind("/opt/", 0) == 0) continue;
            entries.push_back({c.first_seen, "network", c.program + " connected to " + c.remote + ":" + std::to_string(c.port) + " for the first time",
                               "Program: " + c.exe + "\nProtocol: " + c.proto + "\nPID: " + std::to_string(c.pid)});
        }
        if (!entries.empty()) {
            store_->add_log(entries);
            emit logged(int(entries.size()));
        }
    }
    } catch (const std::exception& e) {
        // A watcher must never take the recorder down; say so instead.
        const_cast<as::Snapshot&>(*s).problems.push_back(std::string("Log watchers: ") + e.what());
    }

    // Near the shutdown point every second is written at once, so a sudden
    // power-off still leaves a record of its last moments.
    bool hot = s->guard.margin.ok() && s->guard.margin.get() <= 10;
    if (hot || s->mono - last_flush_ >= 10) {
        store_->add_procs(pendingProcs_);
        pendingProcs_.clear();
        store_->flush();
        last_flush_ = s->mono;
    }
    if (s->mono - last_prune_ >= 3600) {
        store_->rollup_hours(s->wall);  // before prune: it reads the raw samples
        store_->prune(s->wall, hours_, days_);
        store_->prune_log(s->wall, logDays_);
        last_prune_ = s->mono;
    }
    emit sampled(s);

    // Building the culprit list costs a little, so it is only done while the
    // window is open; the tray only needs to know whether an event is running.
    if (watching_ && engine_.has_open() && s->mono - last_open_ >= 2) {
        last_open_ = s->mono;
        auto e = engine_.open_event();
        emit openEvent(e ? std::make_shared<const as::Event>(std::move(*e)) : nullptr, e.has_value());
    } else if (!engine_.has_open() || !watching_) {
        emit openEvent(nullptr, engine_.has_open());
    }
}

namespace {

QString run(const QStringList& args) {
    QProcess p;
    p.start("journalctl", args);
    if (!p.waitForFinished(15000)) {
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput());
}

}  // namespace

// Looks at how earlier boots ended. Each boot is examined once.
void Recorder::checkPastBoots() {
    auto doc = QJsonDocument::fromJson(run({"--list-boots", "-o", "json", "--no-pager"}).toUtf8());
    if (!doc.isArray()) return;
    const double now = double(time(nullptr));
    QJsonArray boots = doc.array();
    // Each boot's first entry by its index: the next boot's start bounds the readings of the one before it.
    std::map<int, double> first;
    for (const auto& v : boots) first[v.toObject().value("index").toInt()] = v.toObject().value("first_entry").toDouble() / 1e6;
    for (const auto& v : boots) {
        QJsonObject b = v.toObject();
        if (b.value("index").toInt() >= 0) continue;  // the current boot
        std::string id = b.value("boot_id").toString().toStdString();
        double end = b.value("last_entry").toDouble() / 1e6;
        const auto next = first.find(b.value("index").toInt() + 1);
        const double next_start = next != first.end() ? next->second : now;
        if (id.empty() || end < now - 30 * 86400 || store_->boot_seen(id)) continue;
        // The kernel's own last words decide a heat shutdown; the overall tail
        // shows whether systemd went through a normal shutdown.
        QString tail = run({"-b", QString::fromStdString(id), "-k", "-n", "40", "--no-pager", "-o", "short-iso"}) +
                       run({"-b", QString::fromStdString(id), "-n", "200", "--no-pager", "-o", "short-iso"});
        if (tail.isEmpty()) continue;  // could not read that boot's log; try again next start
        as::BootEndInfo info = as::classify_boot_end(tail.toStdString());
        store_->mark_boot(id);
        if (info.kind == as::BootEnd::Clean) continue;

        as::Event e = as::shutdown_event(*store_, info, end, next_start);
        int64_t eid = store_->add_event(e);
        emit eventClosed(eid, QString::fromStdString(e.summary));
    }
}
