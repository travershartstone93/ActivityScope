#pragma once
#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <unordered_map>
#include <vector>

#include "snapshot.h"

// Processes as a tree (children under their parent) or a flat list. Rows are
// identified by pid + start time, so selection survives refreshes.
class ProcessModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Col { Name, Pid, User, Status, Cpu, Memory, Disk, Gpu, ColCount };
    explicit ProcessModel(QObject* parent = nullptr) : QAbstractItemModel(parent) {}

    void setSnapshot(const as::SnapshotPtr& s);
    void setTree(bool tree);
    void setFilter(const QString& f);
    void setShowKernel(bool k);
    void sort(int column, Qt::SortOrder order) override;

    const as::ProcessInfo* info(const QModelIndex& i) const;
    QModelIndex indexOfPid(int pid) const;
    double totalMemory() const { return memTotal_; }

    QModelIndex index(int row, int col, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data(const QModelIndex& i, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    struct Node {
        int proc = -1;               // index into snap_->procs; -1 for the root or a section
        int parent = -1;             // node index
        int section = -1;            // section header id, for header nodes
        std::vector<int> children;   // node indices
    };
public:
    enum Section { Apps, Services, System, Kernel, SectionCount };
    static int sectionOf(const as::ProcessInfo& p);
    bool isSection(const QModelIndex& i) const;
    int sectionId(const QModelIndex& i) const;
    QModelIndex sectionIndex(int section) const;
private:
    void rebuild();
    bool matches(const as::ProcessInfo& p) const;
    void sortChildren(int node);
    QIcon iconFor(const as::ProcessInfo& p) const;

    as::SnapshotPtr snap_;
    std::vector<Node> nodes_;
    bool tree_ = true, kernel_ = false;
    QString filter_;
    int sortCol_ = Cpu;
    Qt::SortOrder order_ = Qt::DescendingOrder;
    double memTotal_ = 0;
    mutable QHash<QString, QIcon> icons_;
};
