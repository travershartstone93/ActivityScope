#include "live.h"

#include <QDateTime>
#include <algorithm>
#include <csignal>

#include "actions.h"

Live& Live::get() {
    static Live l;
    return l;
}

QVector<double> Live::series(const as::History& h) {
    QVector<double> v;
    v.reserve(int(h.size()));
    for (std::size_t i = 0; i < h.size(); ++i) v.push_back(h.at(i));
    return v;
}

void Live::add(const as::SnapshotPtr& s) {
    now_ = s;
    cpu.push(s->cpu.total);
    kernel.push(s->cpu.kernel);
    s->cpu.temp_c.ok() ? temp.push(s->cpu.temp_c.get()) : temp.push_gap();
    s->cpu.freq_mhz.ok() ? freq.push(s->cpu.freq_mhz.get()) : freq.push_gap();
    if (cores.size() != s->cpu.per_core.size()) cores.assign(s->cpu.per_core.size(), as::History(kCapacity));
    for (std::size_t i = 0; i < cores.size(); ++i) cores[i].push(s->cpu.per_core[i]);
    mem.push(s->mem.percent());
    memBytes.push(double(s->mem.used));
    swap.push(double(s->mem.swap_total - std::min(s->mem.swap_total, s->mem.swap_free)));
    s->mem.pressure_some10.ok() ? pressure.push(s->mem.pressure_some10.get()) : pressure.push_gap();

    double dtotal = 0, rx = 0, tx = 0;
    for (const auto& d : s->disks) {
        double b = d.read_bps + d.write_bps;
        dtotal += b;
        disk.try_emplace(d.name, kCapacity).first->second.push(b);
    }
    diskBps.push(dtotal);
    for (const auto& n : s->nets)
        if (n.kind != "Loopback" && n.kind != "Virtual") rx += n.rx_bps, tx += n.tx_bps;
    netRx.push(rx);
    netTx.push(tx);
    for (const auto& g : s->gpus) {
        auto& u = gpu.try_emplace(g.pci, kCapacity).first->second;
        g.util.ok() ? u.push(g.util.get()) : u.push_gap();
        auto& m = gpuMem.try_emplace(g.pci, kCapacity).first->second;
        g.mem_used.ok() ? m.push(g.mem_used.get()) : m.push_gap();
    }
    double f = -1;
    for (const auto& fs : s->power.fans)
        if (fs.percent.ok()) f = std::max(f, double(fs.percent.get()));
    f >= 0 ? fan.push(f) : fan.push_gap();
    s->power.battery_w.ok() ? power.push(s->power.battery_w.get()) : power.push_gap();
    s->guard.temp.ok() ? guard.push(s->guard.temp.get()) : guard.push_gap();
    emit updated();
}

bool Live::pause(const as::ProcessKey& key, const QString& name, QString* error) {
    std::string err = as::signal_process(key, SIGSTOP);
    if (!err.empty()) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    paused.push_back({key, name, double(QDateTime::currentSecsSinceEpoch())});
    emit pausedChanged();
    return true;
}

bool Live::resume(const as::ProcessKey& key, QString* error) {
    std::string err = as::signal_process(key, SIGCONT);
    paused.erase(std::remove_if(paused.begin(), paused.end(), [&](const Paused& p) { return p.key == key; }), paused.end());
    emit pausedChanged();
    if (!err.empty() && error) *error = QString::fromStdString(err);
    return err.empty();
}
