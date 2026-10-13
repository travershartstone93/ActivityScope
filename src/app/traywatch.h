#pragma once
#include <QDBusConnection>
#include <QObject>
#include <QSet>
#include <QString>

// Keeps every application's tray icon visible: when an app adds an icon to
// the system tray, it is put on Plasma's "always shown" list, so a program
// cannot sit unnoticed in the hidden part of the tray. Runs on its own thread
// with its own bus connection, so a slow reply can never freeze the window.
class TrayWatch : public QObject {
    Q_OBJECT
public:
    TrayWatch() = default;

public slots:
    void start();

signals:
    void logLine(QString category, QString title, QString detail);

private slots:
    void registered(const QString& service);

private:
    void check(bool announce);
    QString itemProperty(const QString& item, const char* name);
    void keepShown(const QString& id, const QString& title, bool announce);
    QDBusConnection bus_{QString()};
    QString self_;
    QSet<QString> handled_;
    bool primed_ = false;
};
