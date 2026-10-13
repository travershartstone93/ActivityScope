#include "traywatch.h"

#include <QDBusMessage>
#include <QDBusVariant>
#include <QSettings>
#include <QStringList>
#include <QTimer>

namespace {
constexpr int kTimeoutMs = 2000;  // never wait long on another program
}

void TrayWatch::start() {
    bus_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "activityscope-traywatch");
    self_ = QDBusConnection::sessionBus().baseService();  // the app's own tray icon lives here
    bus_.connect(QString(), "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "StatusNotifierItemRegistered", this,
                 SLOT(registered(QString)));
    // The signal does not always arrive, so the list is also checked every
    // 5 s; it is one small bus call.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { check(true); });
    timer->start(5000);
    check(false);
}

QString TrayWatch::itemProperty(const QString& item, const char* name) {
    int slash = item.indexOf('/');
    QString service = slash > 0 ? item.left(slash) : item;
    QString objectPath = slash > 0 ? item.mid(slash) : QStringLiteral("/StatusNotifierItem");
    if (service == self_) return QStringLiteral("ActivityScope");
    QDBusMessage m = QDBusMessage::createMethodCall(service, objectPath, "org.freedesktop.DBus.Properties", "Get");
    m << QStringLiteral("org.kde.StatusNotifierItem") << QString::fromLatin1(name);
    QDBusMessage r = bus_.call(m, QDBus::Block, kTimeoutMs);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) return {};
    return r.arguments().first().value<QDBusVariant>().variant().toString();
}

void TrayWatch::check(bool announce) {
    // With the setting off there is nothing to do; do not interrogate every
    // icon over the bus each poll for nothing.
    if (!QSettings().value("keep_tray_visible", true).toBool()) return;
    QDBusMessage m = QDBusMessage::createMethodCall("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                                                    "org.freedesktop.DBus.Properties", "Get");
    m << QStringLiteral("org.kde.StatusNotifierWatcher") << QStringLiteral("RegisteredStatusNotifierItems");
    QDBusMessage r = bus_.call(m, QDBus::Block, kTimeoutMs);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) return;
    bool all = true;
    for (const QString& item : r.arguments().first().value<QDBusVariant>().variant().toStringList()) {
        QString id = itemProperty(item, "Id");
        if (id.isEmpty()) {  // did not answer: this pass is not fully answered
            all = false;
            continue;
        }
        // Icons that existed before the first fully answered pass (say,
        // before plasmashell was up after login) are kept visible silently;
        // only icons appearing after that are announced as new.
        if (!id.isEmpty() && !handled_.contains(id)) keepShown(id, itemProperty(item, "Title"), announce && primed_);
        if (!id.isEmpty() && !handled_.contains(id)) all = false;
    }
    if (all) primed_ = true;
}

void TrayWatch::registered(const QString& service) {
    QString id = itemProperty(service, "Id");
    if (!id.isEmpty() && !handled_.contains(id)) keepShown(id, itemProperty(service, "Title"), primed_);
}

void TrayWatch::keepShown(const QString& id, const QString& title, bool announce) {
    if (!QSettings().value("keep_tray_visible", true).toBool()) return;
    // Plasma's own scripting interface edits the live panel, so the change
    // applies at once and the panel does not overwrite it later.
    QString quoted = id;
    quoted.replace('\\', "\\\\").replace('"', "\\\"");
    QString script = QString(R"(var added = false;
panels().forEach(function (p) { p.widgets("org.kde.plasma.systemtray").forEach(function (w) {
  w.currentConfigGroup = ["General"];
  var s = w.readConfig("shownItems", []);
  if (typeof s === "string") s = s ? s.split(",") : [];
  if (s.indexOf("%1") < 0) { s.push("%1"); w.writeConfig("shownItems", s); w.reloadConfig(); added = true; }
}); });
print(added ? "added" : "known");)").arg(quoted);
    QDBusMessage m = QDBusMessage::createMethodCall("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript");
    m << script;
    QDBusMessage r = bus_.call(m, QDBus::Block, 5000);
    // Only a real answer from plasmashell counts as handled; if it was not
    // ready (say, right after login) the icon is retried on the next poll.
    if (r.type() != QDBusMessage::ReplyMessage) return;
    handled_.insert(id);
    bool added = !r.arguments().isEmpty() && r.arguments().first().toString().contains("added");
    if ((announce || added) && id != "ActivityScope")
        emit logLine("startup", (title.isEmpty() ? id : title) + " added a tray icon" + (added ? " (kept visible)" : ""),
                     "Tray ID: " + id + "\nActivityScope keeps every application's tray icon on show, so nothing runs unseen in the "
                                        "hidden part of the tray.");
}
