#include <QApplication>
#include <QCommandLineParser>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <unistd.h>

#include "mainwindow.h"
#include "recorder.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("ActivityScope");
    app.setOrganizationName("ActivityScope");
    app.setDesktopFileName("activityscope");
    app.setApplicationVersion(AS_VERSION);
    app.setQuitOnLastWindowClosed(false);  // the tray keeps it alive

    QCommandLineParser args;
    args.addHelpOption();
    QCommandLineOption hidden("hidden", "Start in the tray without opening the window.");
    args.addOption(hidden);
    args.process(app);

    // One copy per user: a second launch asks the first to show its window.
    // (Screenshot runs during development are exempt.)
    const bool dev_shot = qEnvironmentVariableIsSet("AS_SHOT");
    if (dev_shot) app.setOrganizationName("ActivityScope-shot");  // never touch real settings
    const QString key = QString("activityscope-%1").arg(getuid()) + (dev_shot ? "-shot" : "");
    if (!dev_shot) {
        QLocalSocket probe;
        probe.connectToServer(key);
        if (probe.waitForConnected(300)) {
            probe.write(args.isSet(hidden) ? "noop" : "show");
            probe.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer::removeServer(key);
    QLocalServer server;
    server.listen(key);

    qRegisterMetaType<as::SnapshotPtr>();
    qRegisterMetaType<EventPtr>();
    qRegisterMetaType<as::Triggers>();

    QThread worker;
    worker.setObjectName("recorder");
    auto* recorder = new Recorder;
    recorder->moveToThread(&worker);
    QObject::connect(&worker, &QThread::started, recorder, &Recorder::start);
    QObject::connect(&worker, &QThread::finished, recorder, &QObject::deleteLater);
    worker.start(QThread::LowPriority);

    if (dev_shot && qEnvironmentVariableIsSet("AS_THEME")) QSettings().setValue("theme", qEnvironmentVariable("AS_THEME"));
    MainWindow window(recorder);
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
        QLocalSocket* c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::readyRead, &window, [c, &window] {
            if (c->readAll() == "show") window.showFromTray();
            c->deleteLater();
        });
    });
    if (!args.isSet(hidden)) window.show();

    // Development aid: AS_SHOT=file.png [AS_PAGE=n] [AS_DELAY=s] saves a
    // picture of the window and exits, for checking the look.
    if (qEnvironmentVariableIsSet("AS_SHOT")) {
        window.openPage(qEnvironmentVariableIntValue("AS_PAGE"));
        int delay = qEnvironmentVariableIsSet("AS_DELAY") ? qEnvironmentVariableIntValue("AS_DELAY") : 8;
        if (qEnvironmentVariableIsSet("AS_POPOUT")) window.togglePopout();
        const QString press = qEnvironmentVariable("AS_PRESS");
        if (press == "check" || press == "weird")
            for (QObject* o : window.findChildren<QObject*>())
                if (QString(o->metaObject()->className()) == (press == "check" ? "UpdatesPage" : "WeirdPage"))
                    QMetaObject::invokeMethod(o, press == "check" ? "check" : "scan", Qt::QueuedConnection);
        QTimer::singleShot(delay * 1000, &window, [&window] {
            window.grab().save(qEnvironmentVariable("AS_SHOT"));
            if (qEnvironmentVariableIsSet("AS_POPOUT"))
                for (QWidget* w : QApplication::topLevelWidgets())
                    if (w->windowTitle() == "ActivityScope popout") w->grab().save(qEnvironmentVariable("AS_POPOUT"));
            QApplication::quit();
        });
    }

    int rc = app.exec();
    worker.quit();
    worker.wait();
    return rc;
}
