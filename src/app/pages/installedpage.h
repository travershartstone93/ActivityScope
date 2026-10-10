#pragma once
#include <QFutureWatcher>
#include <QWidget>

class QTreeWidget;
class QLineEdit;
class QLabel;

struct AppEntry {
    QString name, comment, exec, icon, file, source, package, version;
    double installed = 0, size = 0;
};

class InstalledPage : public QWidget {
    Q_OBJECT
public:
    explicit InstalledPage(QWidget* parent = nullptr);
    void refresh();

private:
    void fill(const QList<AppEntry>& apps);
    QTreeWidget* tree_;
    QLineEdit* filter_;
    QLabel* count_;
    QFutureWatcher<QList<AppEntry>> watcher_;
    bool started_ = false;
};
