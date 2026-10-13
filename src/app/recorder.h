#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <ctime>
#include <set>
#include <memory>

#include "events.h"
#include "sampler.h"
#include "store.h"
#include "netlog.h"
#include "peerwatch.h"
#include "persist.h"
#include "processlog.h"
#include "watch.h"

class QTimer;

using EventPtr = std::shared_ptr<const as::Event>;
Q_DECLARE_METATYPE(as::SnapshotPtr)
Q_DECLARE_METATYPE(EventPtr)
Q_DECLARE_METATYPE(as::Triggers)

// Runs on its own thread: samples, detects events, writes history, feeds conky.
class Recorder : public QObject {
    Q_OBJECT
public:
    Recorder() = default;
    ~Recorder() override;

public slots:
    void start();
    void setWatching(bool watching) { watching_ = watching; }
    void setTriggers(const as::Triggers& t) { engine_.set_triggers(t); }
    void setInterval(int ms);
    void addLog(const QString& category, const QString& title, const QString& detail) {
        if (!store_) return;
        store_->add_log({{double(time(nullptr)), category.toStdString(), title.toStdString(), detail.toStdString()}});
        emit logged(1);
    }
    void setAllowedSharers(const QStringList& names) {
        allowedSharers_.clear();
        for (const auto& n : names) allowedSharers_.insert(n.toStdString());
    }
    void setRetention(double hours, double days, double logDays = 90) {
        hours_ = hours;
        days_ = days;
        logDays_ = logDays;
    }

signals:
    void sampled(as::SnapshotPtr snapshot);
    void openEvent(EventPtr event, bool open);  // event is null when only the flag is known
    void eventClosed(qint64 id, QString summary);
    void storeError(QString message);
    void logged(int count);  // new lines in the investigation log
    // A program is sharing with many computers in the background.
    void sharing(QString program, int pid, quint64 startTicks, int peers, double uploadBps);

private:
    void tick();
    void checkPastBoots();
    QTimer* timer_ = nullptr;
    std::unique_ptr<as::Sampler> sampler_;
    std::unique_ptr<as::Store> store_;
    std::unique_ptr<as::LogWatch> logwatch_;
    std::unique_ptr<as::StartupWatch> startup_;
    as::ProcessLog proclog_;
    as::NetLog netlog_;
    as::PeerWatch peers_;
    std::set<std::string> allowedSharers_;
    std::vector<as::ProcessRecord> pendingProcs_;
    double logDays_ = 90;
    as::EventEngine engine_;
    bool watching_ = false;
    double last_flush_ = 0, last_prune_ = 0, last_open_ = 0;
    double hours_ = 24, days_ = 30;
    std::string feed_dir_;
};
