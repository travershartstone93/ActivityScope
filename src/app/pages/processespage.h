#pragma once
#include <QWidget>
#include <set>

#include "snapshot.h"

class ProcessModel;
class QTreeView;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QGridLayout;

class ProcessesPage : public QWidget {
    Q_OBJECT
public:
    explicit ProcessesPage(QWidget* parent = nullptr);
    void refresh();
    void selectPid(int pid);

private:
    void apply(const as::SnapshotPtr& s);
    void showDetails();
    void endSelected(bool force);
    void remember();
    void restore();

    ProcessModel* model_;
    QTreeView* view_;
    QLineEdit* filter_;
    QComboBox* mode_;
    QWidget* details_;
    QLabel *dTitle_, *dPath_;
    QGridLayout* dGrid_;
    QPushButton *end_, *kill_, *pause_;
    QLabel* actionMsg_;
    as::ProcessKey selected_{};
    std::vector<as::ProcessKey> expanded_;
    std::set<int> collapsedSections_;
    double lastApply_ = 0;
    uint64_t lastGen_ = 0;
    int sortCol_ = 4;  // ProcessModel::Cpu: the column the list is sorted by, to tell a first click from a flip
};
