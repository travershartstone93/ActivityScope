#include "processespage.h"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <functional>
#include <unistd.h>

#include "../format.h"
#include "../icons.h"
#include "../live.h"
#include "../theme.h"
#include "actions.h"
#include "processmodel.h"

namespace {

// The CPU column paints a soft bar behind the number, brighter as it climbs.
class CpuBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& i) const override {
        double v = i.data(Qt::UserRole).toDouble();
        if (v >= 0.3) {
            // Bar length is the share of the whole machine, stretched so that
            // even a few percent is visible.
            QRectF r = QRectF(opt.rect).adjusted(8, 5, -6, -5);
            double frac = std::clamp(std::sqrt(v / 100.0), 0.04, 1.0);
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            QColor c = v > 25 ? pal().alert : v > 8 ? pal().temp : pal().cpu;
            p->setBrush(withAlpha(c, 40));
            p->drawRoundedRect(r, 3, 3);
            p->setBrush(withAlpha(c, 150));
            p->drawRoundedRect(QRectF(r.left(), r.top(), r.width() * frac, r.height()), 3, 3);
            p->restore();
        }
        QStyledItemDelegate::paint(p, opt, i);
    }
};

QLabel* sectionLabel(const QString& t) {
    auto* l = new QLabel(t);
    l->setStyleSheet(QString("color:%1; font-size:13px; font-weight:600;").arg(pal().text.name()));
    return l;
}

}  // namespace

ProcessesPage::ProcessesPage(QWidget* parent) : QWidget(parent) {
    const Palette& P = pal();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 12);
    v->setSpacing(10);

    auto* bar = new QHBoxLayout;
    mode_ = new QComboBox;
    mode_->addItems({"Process tree", "Flat list", "Flat list with kernel threads"});
    filter_ = new QLineEdit;
    filter_->setPlaceholderText("Filter by name, user, command or PID");
    filter_->addAction(themedIcon("search", P.dim), QLineEdit::LeadingPosition);
    filter_->setClearButtonEnabled(true);
    filter_->setMinimumWidth(360);
    bar->addStretch();
    bar->addWidget(mode_);
    bar->addWidget(filter_);
    v->addLayout(bar);

    model_ = new ProcessModel(this);
    view_ = new QTreeView;
    view_->setModel(model_);
    view_->setSortingEnabled(true);
    view_->sortByColumn(ProcessModel::Cpu, Qt::DescendingOrder);
    view_->setAlternatingRowColors(true);
    view_->setUniformRowHeights(true);
    view_->setIconSize(QSize(18, 18));
    view_->setItemDelegateForColumn(ProcessModel::Cpu, new CpuBarDelegate(view_));
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setStyleSheet("QTreeView { font-size: 14px; } QTreeView::item { height: 30px; }");
    auto* hh = view_->header();
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(ProcessModel::Name, QHeaderView::Stretch);
    for (int c = 1; c < ProcessModel::ColCount; ++c) hh->setSectionResizeMode(c, QHeaderView::Interactive);
    hh->resizeSection(ProcessModel::Pid, 90);
    hh->resizeSection(ProcessModel::User, 110);
    hh->resizeSection(ProcessModel::Status, 140);
    hh->resizeSection(ProcessModel::Cpu, 140);
    hh->resizeSection(ProcessModel::Memory, 120);
    hh->resizeSection(ProcessModel::Disk, 120);
    hh->resizeSection(ProcessModel::Gpu, 120);
    v->addWidget(view_, 1);

    details_ = new QWidget;
    details_->setObjectName("details");
    details_->setStyleSheet(QString("#details { background:%1; border:1px solid %2; border-radius:9px; }").arg(P.panel.name(), P.border.name(QColor::HexArgb)));
    auto* dv = new QVBoxLayout(details_);
    dv->setContentsMargins(18, 14, 18, 14);
    auto* top = new QHBoxLayout;
    dTitle_ = new QLabel("Select a process");
    dTitle_->setMinimumHeight(30);
    dTitle_->setStyleSheet("font-size:19px; font-weight:700;");
    end_ = new QPushButton("End process");
    kill_ = new QPushButton("Force end");
    kill_->setObjectName("danger");
    end_->setEnabled(false);
    kill_->setEnabled(false);
    pause_ = new QPushButton("Pause");
    pause_->setEnabled(false);
    pause_->setToolTip("Freeze the process without ending it, for example to let the machine cool. Resume carries on where it stopped.");
    top->addWidget(dTitle_, 1);
    top->addWidget(pause_);
    top->addWidget(end_);
    top->addWidget(kill_);
    dPath_ = new QLabel;
    dPath_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
    dPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dPath_->setWordWrap(true);
    actionMsg_ = new QLabel;
    actionMsg_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.temp.name()));
    actionMsg_->hide();
    dGrid_ = new QGridLayout;
    dGrid_->setHorizontalSpacing(18);
    dGrid_->setVerticalSpacing(5);
    dv->addLayout(top);
    dv->addWidget(dPath_);
    dv->addWidget(actionMsg_);
    dv->addSpacing(6);
    dv->addLayout(dGrid_);
    dv->addStretch();
    details_->setFixedHeight(230);
    v->addWidget(details_);

    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int m) {
        remember();
        model_->setShowKernel(m == 2);
        model_->setTree(m == 0);
        restore();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) {
        remember();
        model_->setFilter(f);
        if (!f.isEmpty() && mode_->currentIndex() == 0) view_->expandAll();
        restore();
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& i) {
        if (const auto* p = model_->info(i)) {
            if (!(p->key == selected_)) actionMsg_->hide();
            selected_ = p->key;
        }
        showDetails();
    });
    connect(view_->header(), &QHeaderView::sortIndicatorChanged, this, [this](int col, Qt::SortOrder order) {
        // A first click on a figure column asks for the biggest first, the way a
        // task manager reads; Qt's own first click would put the idle ones on top.
        const bool figure = col == ProcessModel::Cpu || col == ProcessModel::Memory || col == ProcessModel::Disk ||
                            col == ProcessModel::Gpu;
        const bool fresh = col != sortCol_;
        sortCol_ = col;
        if (figure && fresh && order == Qt::AscendingOrder) {
            view_->header()->setSortIndicator(col, Qt::DescendingOrder);  // comes back through here, descending
            return;
        }
        // The sort rebuilt the model, which collapses every row; reopen what was open
        // at the last refresh (remember() now would only see the collapse).
        restore();
    });
    connect(pause_, &QPushButton::clicked, this, [this] {
        const auto& s = Live::get().now();
        if (!s) return;
        for (const auto& q : s->procs) {
            if (!(q.key == selected_)) continue;
            QString err;
            bool stopped = q.state == "T" || std::any_of(Live::get().paused.begin(), Live::get().paused.end(),
                                                         [&](const auto& p) { return p.key == selected_; });
            bool ok = stopped ? Live::get().resume(selected_, &err) : Live::get().pause(selected_, qs(q.name), &err);
            actionMsg_->setText(ok ? (stopped ? qs(q.name) + " is running again." : qs(q.name) + " is paused. Resume it from here or from Summary.") : err);
            actionMsg_->show();
            pause_->setText(stopped ? "Pause" : "Resume");
        }
    });
    connect(end_, &QPushButton::clicked, this, [this] { endSelected(false); });
    connect(kill_, &QPushButton::clicked, this, [this] { endSelected(true); });
}

void ProcessesPage::remember() {
    expanded_.clear();
    for (int sec = 0; sec < ProcessModel::SectionCount; ++sec) {
        QModelIndex h = model_->sectionIndex(sec);
        if (h.isValid() && !view_->isExpanded(h)) collapsedSections_.insert(sec);
        else if (h.isValid()) collapsedSections_.erase(sec);
    }
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < model_->rowCount(parent); ++r) {
            QModelIndex i = model_->index(r, 0, parent);
            if (model_->isSection(i)) {
                walk(i);
                continue;
            }
            if (view_->isExpanded(i)) {
                if (const auto* p = model_->info(i)) expanded_.push_back(p->key);
                walk(i);
            }
        }
    };
    walk({});
}

void ProcessesPage::restore() {
    for (int sec = 0; sec < ProcessModel::SectionCount; ++sec) {
        QModelIndex h = model_->sectionIndex(sec);
        if (h.isValid()) view_->setExpanded(h, !collapsedSections_.count(sec));
    }
    for (const auto& k : expanded_) {
        QModelIndex i = model_->indexOfPid(k.pid);
        if (i.isValid()) view_->setExpanded(i, true);
    }
    QModelIndex sel = model_->indexOfPid(selected_.pid);
    if (sel.isValid()) {
        QSignalBlocker block(view_->selectionModel());
        view_->selectionModel()->setCurrentIndex(sel, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    showDetails();
}

void ProcessesPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || s->generation == lastGen_) return;
    // A full scan lands at most every second; redrawing the table every
    // two seconds keeps it readable while still current.
    if (s->mono - lastApply_ < 1.9 && lastApply_ > 0) {
        showDetails();
        return;
    }
    lastGen_ = s->generation;
    lastApply_ = s->mono;
    apply(s);
}

void ProcessesPage::apply(const as::SnapshotPtr& s) {
    int scroll = view_->verticalScrollBar()->value();
    remember();
    model_->setSnapshot(s);
    // Open with the busiest of your own programs selected, so the details
    // panel has something to say from the start.
    if (selected_.pid == 0) {
        const as::ProcessInfo* best = nullptr;
        for (const auto& p : s->procs)
            if (!p.kernel_thread && !p.self && p.uid == int(getuid()) && p.name != "systemd" && (!best || p.cpu > best->cpu)) best = &p;
        if (best && best->cpu > 0) selected_ = best->key;  // the first scan has no CPU readings yet
    }
    restore();
    view_->verticalScrollBar()->setValue(scroll);
}

void ProcessesPage::selectPid(int pid) {
    if (const auto& s = Live::get().now()) apply(s);
    filter_->clear();
    QModelIndex i = model_->indexOfPid(pid);
    if (!i.isValid()) return;
    for (QModelIndex p = i.parent(); p.isValid(); p = p.parent()) view_->expand(p);
    view_->setCurrentIndex(i);
    view_->scrollTo(i, QAbstractItemView::PositionAtCenter);
}

void ProcessesPage::showDetails() {
    while (QLayoutItem* it = dGrid_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const auto& s = Live::get().now();
    const as::ProcessInfo* p = nullptr;
    if (s)
        for (const auto& q : s->procs)
            if (q.key == selected_) p = &q;
    end_->setEnabled(p && !p->kernel_thread && !p->self);
    pause_->setEnabled(p && !p->kernel_thread && !p->self);
    if (p) {
        bool stopped = p->state == "T" || std::any_of(Live::get().paused.begin(), Live::get().paused.end(),
                                                      [&](const auto& x) { return x.key == p->key; });
        pause_->setText(stopped ? "Resume" : "Pause");
    }
    kill_->setEnabled(p && !p->kernel_thread && !p->self);
    if (!p) {
        dTitle_->setText(selected_.pid ? QString("PID %1 has ended").arg(selected_.pid) : "Select a process");
        dPath_->setText(selected_.pid ? "It is no longer running." : "Click a row to see what it is and what it is using.");
        return;
    }
    dTitle_->setText(QString("%1  (%2)").arg(qs(p->name)).arg(p->key.pid));
    QString path = qs(p->exe.empty() ? p->cmdline : p->exe);
    if (!p->cmdline.empty() && qs(p->cmdline) != path) path += "\n" + qs(p->cmdline).left(400);
    dPath_->setText(path.isEmpty() ? "Kernel thread: part of the operating system itself." : path);

    QString parent = QString::number(p->ppid);
    for (const auto& q : s->procs)
        if (q.key.pid == p->ppid) parent = qs(q.name) + QString(" (%1)").arg(p->ppid);
    QDateTime started = QDateTime::fromSecsSinceEpoch(qint64(p->started_epoch));
    double up = s->wall - p->started_epoch;

    struct Col { QString head; QList<QPair<QString, QString>> rows; };
    QList<Col> cols = {
        {"Identity", {{"Parent", parent}, {"User", qs(p->user)}, {"Unit", p->unit.empty() ? "-" : qs(p->unit)}, {"Status", model_->data(model_->indexOfPid(p->key.pid).siblingAtColumn(ProcessModel::Status), Qt::DisplayRole).toString()}}},
        {"Lifetime", {{"Started", started.toString("MMM d, h:mm:ss AP")}, {"Running for", duration(up)}, {"Threads", QString::number(p->threads)}, {"Priority", QString("nice %1").arg(p->nice)}}},
        {"Processor", {{"CPU now", pct(p->cpu)}, {"Children", pct(p->child_cpu)}, {"CPU time", duration(p->cpu_seconds)}, {"GPU memory", p->gpu_mem.ok() ? bytes(p->gpu_mem.get()) : "None"}}},
        {"Memory", {{"Resident", bytes(double(p->rss))}, {"Share of RAM", pct(model_->totalMemory() ? 100.0 * double(p->rss) / model_->totalMemory() : 0)}, {"Major faults", QString::number(p->major_faults)}}},
        {"Disk", {{"Reading", p->read_bps.ok() ? rate(p->read_bps.get()) : "Hidden"}, {"Writing", p->write_bps.ok() ? rate(p->write_bps.get()) : "Hidden"}}},
    };
    for (int c = 0; c < cols.size(); ++c) {
        dGrid_->addWidget(sectionLabel(cols[c].head), 0, c * 2, 1, 2);
        for (int r = 0; r < cols[c].rows.size(); ++r) {
            auto* k = new QLabel(cols[c].rows[r].first);
            k->setStyleSheet(QString("color:%1;").arg(pal().dim.name()));
            auto* val = new QLabel(cols[c].rows[r].second);
            val->setToolTip(cols[c].head == "Disk" && !p->read_bps.ok() ? qs(p->read_bps.why) : QString());
            dGrid_->addWidget(k, r + 1, c * 2);
            dGrid_->addWidget(val, r + 1, c * 2 + 1);
        }
    }
    for (int c = 0; c < cols.size(); ++c) dGrid_->setColumnStretch(c * 2 + 1, 1);
}

void ProcessesPage::endSelected(bool force) {
    const auto& s = Live::get().now();
    if (!s) return;
    QString name;
    for (const auto& q : s->procs)
        if (q.key == selected_) name = qs(q.name);
    if (name.isEmpty()) return;
    if (force) {
        auto answer = QMessageBox::question(this, "Force end " + name,
                                            QString("Force-ending %1 stops it immediately without letting it save anything. Continue?").arg(name));
        if (answer != QMessageBox::Yes) return;
    }
    std::string err = as::signal_process(selected_, force ? SIGKILL : SIGTERM);
    actionMsg_->setText(err.empty() ? (force ? name + " was force-ended." : "Asked " + name + " to close. If it does not, use Force end.")
                                    : qs(err));
    actionMsg_->show();
}
