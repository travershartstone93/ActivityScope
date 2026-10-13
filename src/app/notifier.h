#pragma once
#include <QHash>
#include <QObject>

#include "snapshot.h"

// Warns before the kernel's thermal shutdown with a critical desktop
// notification, and lets the user pause the process driving the heat.
class Notifier : public QObject {
    Q_OBJECT
public:
    explicit Notifier(QObject* parent = nullptr);
    void onSample(const as::SnapshotPtr& s);
    void sharing(const QString& program, int pid, quint64 startTicks, int peers, double uploadBps);
    static const as::ProcessInfo* heatCulprit(const as::Snapshot& s, bool* pausable = nullptr);
    static bool danger(const as::Snapshot& s);

signals:
    void openRequested();
    void allowSharer(const QString& program);

private slots:
    void onAction(uint id, const QString& action);
    void onClosed(uint id, uint reason);

private:
    uint notify(const QString& title, const QString& body, const QStringList& actions, bool critical, uint replaces = 0);
    struct Target { as::ProcessKey key; QString name; };
    QHash<uint, Target> targets_;  // notification id -> process its buttons act on
    uint warningId_ = 0;
    bool warning_ = false;
    int dangerStreak_ = 0, calmStreak_ = 0;
    double lastSent_ = 0, lastMargin_ = 1e9;
};
