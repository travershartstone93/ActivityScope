#pragma once
#include <QJsonDocument>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QProgressBar;

// A GUI for the weird command: every scan is a button press, nothing runs on
// its own, and the judgements shown are weird's, not ActivityScope's.
class WeirdPage : public QWidget {
    Q_OBJECT
public:
    explicit WeirdPage(QWidget* parent = nullptr);
    void refresh() {}

signals:
    void showProcess(int pid);

public slots:
    void scan();

private:
    void fill(const QJsonDocument& doc);
    void showDetail(QTreeWidgetItem* item);
    QString weirdPath() const;
    bool usingBundled() const;

    QLabel *status_, *headline_, *detail_, *coverage_;
    QPushButton *scanBtn_, *trustBtn_, *jumpBtn_;
    QProgressBar* progress_;
    QTreeWidget* tree_;
    bool scanning_ = false;
};
