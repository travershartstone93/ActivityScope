#pragma once
#include <QWidget>
#include <atomic>
#include <memory>
#include <thread>

#include "dirscan.h"

using as::DirNode;

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QVBoxLayout;


class DiskSpacePage : public QWidget {
    Q_OBJECT
public:
    explicit DiskSpacePage(QWidget* parent = nullptr);
    ~DiskSpacePage() override;
    void refresh();

private:
    void startScan(const QString& root);
    void showTree();
    void addChildren(QTreeWidgetItem* parent, const DirNode& node, double total);

    QVBoxLayout* drives_;
    QTreeWidget* tree_;
    QLabel *status_;
    QPushButton *scan_, *choose_;
    QString root_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::atomic<long long> scannedFiles_{0}, scannedBytes_{0};
    std::unique_ptr<DirNode> result_;
    double drivesAt_ = 0;
};
