#include "recorder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "conkyfeed.h"
#include "guard.h"

Recorder::~Recorder() {
    if (store_) store_->flush();
}

void Recorder::start() {
    sampler_ = std::make_unique<as::Sampler>();
    store_ = std::make_unique<as::Store>(as::default_db_path());
    if (!store_->ok()) emit storeError(QString::fromStdString(store_->error()));
    feed_dir_ = as::runtime_dir();
    checkPastBoots();
    logwatch_ = std::make_unique<as::LogWatch>(store_.get());
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
    if (logwatch_) {
        auto entries = logwatch_->poll(*s);
        if (!entries.empty()) {
            store_->add_log(entries);
            emit logged(int(entries.size()));
        }
    }

    // Near the shutdown point every second is written at once, so a sudden
    // power-off still leaves a record of its last moments.
    bool hot = s->guard.margin.ok() && s->guard.margin.get() <= 10;
    if (hot || s->mono - last_flush_ >= 10) {
        store_->flush();
        last_flush_ = s->mono;
    }
    if (s->mono - last_prune_ >= 3600) {
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

std::string clock_date(double t) {
    time_t tt = time_t(t);
    tm lt{};
    localtime_r(&tt, &lt);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a %b %e at %H:%M:%S", &lt);
    return buf;
}

}  // namespace

// Looks at how earlier boots ended. Each boot is examined once.
void Recorder::checkPastBoots() {
    auto doc = QJsonDocument::fromJson(run({"--list-boots", "-o", "json", "--no-pager"}).toUtf8());
    if (!doc.isArray()) return;
    const double now = double(time(nullptr));
    QJsonArray boots = doc.array();
    for (const auto& v : boots) {
        QJsonObject b = v.toObject();
        if (b.value("index").toInt() >= 0) continue;  // the current boot
        std::string id = b.value("boot_id").toString().toStdString();
        double end = b.value("last_entry").toDouble() / 1e6;
        if (id.empty() || end < now - 30 * 86400 || store_->boot_seen(id)) continue;
        // The kernel's own last words decide a heat shutdown; the overall tail
        // shows whether systemd went through a normal shutdown.
        QString tail = run({"-b", QString::fromStdString(id), "-k", "-n", "40", "--no-pager", "-o", "short-iso"}) +
                       run({"-b", QString::fromStdString(id), "-n", "200", "--no-pager", "-o", "short-iso"});
        if (tail.isEmpty()) continue;  // could not read that boot's log; try again next start
        as::BootEndInfo info = as::classify_boot_end(tail.toStdString());
        store_->mark_boot(id);
        if (info.kind == as::BootEnd::Clean) continue;

        as::Event e;
        e.start = e.end = e.last_active = end;
        e.kinds = as::KindShutdown | (info.kind == as::BootEnd::Thermal ? as::KindHeat : 0u);
        e.primary = as::KindShutdown;
        e.open = false;
        if (!info.line.empty()) e.triggers.push_back(info.line.substr(info.line.find(' ') + 1));
        std::string when = clock_date(end);
        std::string text;
        if (info.kind == as::BootEnd::Thermal)
            text = "The system shut itself down for heat on " + when + ": the kernel's shutdown sensor reached its critical temperature.";
        else if (info.kind == as::BootEnd::Asleep)
            text = "The machine went to sleep on " + when + " and never woke up; the battery may have run out.";
        else
            text = "The machine stopped without warning on " + when +
                   ". Nothing was written to the system log first, which is what happens when the laptop's own firmware cuts the power, "
                   "the battery dies, or the system freezes.";

        as::StoredSample last = store_->last_sample_before(end + 2);
        if (last.t > 0 && end - last.t < 120) {
            char buf[256];
            if (last.guard >= 0 && last.guard_limit > 0) {
                std::snprintf(buf, sizeof buf, " The last reading, %.0f s before, was %.0f °C on the shutdown sensor (%.0f °C below its %.0f °C limit), with the CPU %.0f%% busy.",
                              end - last.t, last.guard, last.guard_limit - last.guard, last.guard_limit, last.cpu);
                e.min_margin = std::max(0.0, last.guard_limit - last.guard);
                e.guard_limit = last.guard_limit;
            } else {
                std::snprintf(buf, sizeof buf, " The last reading, %.0f s before, had the CPU at %.0f °C and %.0f%% busy.", end - last.t, last.temp, last.cpu);
            }
            text += buf;
            for (const auto& x : store_->samples(end - 120, end + 2)) {
                as::EventSample p;
                p.t = x.t;
                p.cpu = x.cpu;
                p.mem_pct = x.mem_pct;
                p.temp = x.temp;
                p.fan = x.fan;
                p.guard = x.guard;
                e.samples.push_back(p);
                e.peak_cpu = std::max(e.peak_cpu, x.cpu);
                e.peak_temp = std::max(e.peak_temp, x.temp);
                e.peak_fan = std::max(e.peak_fan, x.fan);
                if (x.guard >= 0 && x.guard_limit > 0) {
                    double m = std::max(0.0, x.guard_limit - x.guard);
                    if (e.min_margin < 0 || m < e.min_margin) e.min_margin = m;
                }
            }
            auto top = store_->minute_top(end - 120, end + 2);
            double total = 0;
            for (const auto& c : top) total += c.cpu_seconds;
            for (auto& c : top) c.share = total > 0 ? c.cpu_seconds / total : 0;
            if (top.size() > 8) top.resize(8);
            if (!top.empty() && top.front().share >= 0.2) {
                std::snprintf(buf, sizeof buf, " The busiest program in its last two minutes was %s (%.0f%% of the CPU time).", top.front().name.c_str(),
                              top.front().share * 100);
                text += buf;
            }
            e.culprits = std::move(top);
        } else {
            text += " ActivityScope was not recording then, so there are no readings from just before.";
        }
        e.summary = std::string(as::clock_time(end)) + " to " + as::clock_time(end) + " (1 s). " + text;
        int64_t eid = store_->add_event(e);
        emit eventClosed(eid, QString::fromStdString(e.summary));
    }
}
