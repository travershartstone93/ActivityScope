#pragma once
#include <QObject>
#include <QString>
#include <memory>

#include "events.h"
#include "sampler.h"
#include "store.h"
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

private:
    void tick();
    void checkPastBoots();
    QTimer* timer_ = nullptr;
    std::unique_ptr<as::Sampler> sampler_;
    std::unique_ptr<as::Store> store_;
    std::unique_ptr<as::LogWatch> logwatch_;
    double logDays_ = 90;
    as::EventEngine engine_;
    bool watching_ = false;
    double last_flush_ = 0, last_prune_ = 0, last_open_ = 0;
    double hours_ = 24, days_ = 30;
    std::string feed_dir_;
};
