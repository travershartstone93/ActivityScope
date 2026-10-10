#pragma once
#include <QObject>
#include <QVector>
#include <map>
#include <string>

#include "history.h"
#include "recorder.h"

// The window's view of the recent past: one ring per line it draws.
class Live : public QObject {
    Q_OBJECT
public:
    static Live& get();
    static constexpr int kCapacity = 120;

    void add(const as::SnapshotPtr& s);
    const as::SnapshotPtr& now() const { return now_; }
    static QVector<double> series(const as::History& h);

    as::History cpu{kCapacity}, kernel{kCapacity}, temp{kCapacity}, freq{kCapacity};
    std::vector<as::History> cores;
    as::History mem{kCapacity}, memBytes{kCapacity}, swap{kCapacity}, pressure{kCapacity};
    as::History diskBps{kCapacity}, netRx{kCapacity}, netTx{kCapacity};
    as::History fan{kCapacity}, power{kCapacity}, guard{kCapacity};
    std::map<std::string, as::History> disk, gpu, gpuMem;

    // Processes paused from a shutdown warning, until the user resumes them.
    struct Paused { as::ProcessKey key; QString name; double when; };
    std::vector<Paused> paused;
    bool pause(const as::ProcessKey& key, const QString& name, QString* error = nullptr);
    bool resume(const as::ProcessKey& key, QString* error = nullptr);

    EventPtr openEvent;
    bool eventOpen = false;
    int eventsToday = 0;

signals:
    void updated();
    void eventChanged();
    void pausedChanged();

private:
    Live() = default;
    as::SnapshotPtr now_;
};
