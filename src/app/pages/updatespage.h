#pragma once
#include <QWidget>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "updates.h"
#include "upgradecheck.h"

class QTreeWidget;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QProgressBar;

// Updates on request only: nothing here runs until you press a button.
class UpdatesPage : public QWidget {
    Q_OBJECT
public:
    explicit UpdatesPage(QWidget* parent = nullptr);
    ~UpdatesPage() override;
    void refresh() {}

public slots:
    void check();

private:
    void finishCheck();
    void fill();
    void analyse();
    void setBusy(bool busy, const QString& what = {}, int percent = -1);
    void showAlerts();
    QString checkDb() const;
    void runInTerminal(const QString& what, const QString& command);
    std::set<std::string> held() const;
    std::set<std::string> heldBySystem() const;
    void setHeld(const std::string& name, bool hold);
    double lastFullUpgrade() const;

    QLabel *status_, *holds_;
    QPushButton *checkBtn_, *allBtn_;
    QTreeWidget* tree_;
    QWidget* alertBox_;
    QWidget* progressBox_;
    QProgressBar* progress_;
    QLabel* progressLabel_;
    int updatable_ = 0;
    bool recheckOnClose_ = false;
    QVBoxLayout* alerts_;
    as::CheckResult result_;
    bool analysing_ = false, analysed_ = false;
    std::vector<as::PendingUpdate> updates_;
    int pending_ = 0;
    bool checked_ = false;
    QStringList checkErrors_;
    // The analysis runs on a pool thread; this keeps it from touching the
    // page after the page is gone.
    struct Guard { std::mutex m; UpdatesPage* page = nullptr; };
    std::shared_ptr<Guard> guard_;
};
