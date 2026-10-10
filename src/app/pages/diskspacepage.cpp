#include "diskspacepage.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>

#include "../format.h"
#include "../live.h"
#include "../widgets/card.h"
#include "../widgets/segmentmeter.h"
#include "common.h"


DiskSpacePage::DiskSpacePage(QWidget* parent) : QWidget(parent), root_(QDir::homePath()) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(12);
    auto* drivesCard = new Card("hard-drive", "Drives");
    drivesCard->setTitleSize(20);
    drives_ = new QVBoxLayout;
    drives_->setSpacing(8);
    drivesCard->body()->addLayout(drives_);
    v->addWidget(drivesCard);

    auto* useCard = new Card("chart-pie", "What is using the space");
    useCard->setTitleSize(20);
    auto* bar = new QHBoxLayout;
    status_ = noteLabel("Choose a folder and scan it to see which folders inside are biggest.");
    choose_ = new QPushButton("Choose folder");
    scan_ = new QPushButton("Scan home folder");
    bar->addWidget(status_, 1);
    bar->addWidget(choose_);
    bar->addWidget(scan_);
    tree_ = styledTree({"Folder", "Size", "Share", "Files"});
    tree_->setRootIsDecorated(true);
    tree_->setSortingEnabled(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->resizeSection(1, 120);
    tree_->header()->resizeSection(2, 160);
    tree_->header()->resizeSection(3, 110);
    useCard->body()->addLayout(bar);
    useCard->body()->addWidget(tree_, 1);
    v->addWidget(useCard, 1);

    connect(choose_, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Folder to scan", root_);
        if (d.isEmpty()) return;
        root_ = d;
        scan_->setText("Scan " + QDir(d).dirName());
    });
    connect(scan_, &QPushButton::clicked, this, [this] {
        if (worker_.joinable() && !done_) {
            cancel_ = true;
            return;
        }
        startScan(root_);
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        // Children are filled in when a folder is opened, to keep big trees quick.
        if (it->childCount() == 1 && it->child(0)->text(0).isEmpty()) {
            delete it->takeChild(0);
            auto* node = static_cast<DirNode*>(it->data(0, Qt::UserRole).value<void*>());
            if (node) addChildren(it, *node, node->bytes);
        }
    });
}

DiskSpacePage::~DiskSpacePage() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void DiskSpacePage::refresh() {
    const auto& s = Live::get().now();
    if (!s) return;
    if (worker_.joinable() && !done_)
        status_->setText(QString("Scanning %1: %2 files, %3 so far...").arg(root_).arg(scannedFiles_.load()).arg(bytes(double(scannedBytes_.load()))));
    if (s->wall - drivesAt_ < 30 && drivesAt_ > 0) return;
    drivesAt_ = s->wall;
    while (QLayoutItem* it = drives_->takeAt(0)) {
        if (it->layout())
            while (QLayoutItem* c = it->layout()->takeAt(0)) {
                delete c->widget();
                delete c;
            }
        delete it->layout();
        delete it->widget();
        delete it;
    }
    QFile mounts("/proc/self/mounts");
    if (!mounts.open(QIODevice::ReadOnly)) return;
    QSet<QString> seenDev;
    for (const auto& line : QString::fromUtf8(mounts.readAll()).split('\n')) {
        auto f = line.split(' ');
        if (f.size() < 3 || !f[0].startsWith("/dev/") || f[0].startsWith("/dev/loop")) continue;
        if (seenDev.contains(f[0])) continue;  // btrfs subvolumes share one device
        seenDev.insert(f[0]);
        QString mp = f[1].replace("\\040", " ");
        struct statvfs v {};
        if (statvfs(mp.toLocal8Bit().constData(), &v) != 0 || v.f_blocks == 0) continue;
        double total = double(v.f_blocks) * v.f_frsize, freeB = double(v.f_bavail) * v.f_frsize, used = total - double(v.f_bfree) * v.f_frsize;
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(QString("<b>%1</b><br><span style='color:%2'>%3, %4</span>").arg(mp, pal().dim.name(), f[0], f[2]));
        name->setMinimumWidth(260);
        auto* meter = new SegmentMeter(Qt::Horizontal);
        meter->setFixedHeight(26);
        meter->setSegmentSize(10);
        meter->setColor(used / total > 0.9 ? pal().alert : pal().disk);
        meter->setValue(used / total);
        auto* text = new QLabel(QString("%1 used of %2, %3 free").arg(bytes(used), bytes(total), bytes(freeB)));
        text->setMinimumWidth(280);
        text->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(name);
        row->addWidget(meter, 1);
        row->addWidget(text);
        drives_->addLayout(row);
    }
}

void DiskSpacePage::startScan(const QString& root) {
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    done_ = false;
    scannedFiles_ = 0;
    scannedBytes_ = 0;
    tree_->clear();
    scan_->setText("Stop");
    choose_->setEnabled(false);
    worker_ = std::thread([this, root] {
        result_ = as::scan_dir(root.toStdString(), cancel_, scannedFiles_, scannedBytes_);
        done_ = true;
        QMetaObject::invokeMethod(this, [this] { showTree(); }, Qt::QueuedConnection);
    });
}

void DiskSpacePage::showTree() {
    if (worker_.joinable()) worker_.join();
    scan_->setText("Scan " + QDir(root_).dirName());
    choose_->setEnabled(true);
    if (!result_) return;
    status_->setText(QString("%1%2: %3 in %4 files.")
                         .arg(cancel_ ? "Stopped early. " : "", root_, bytes(result_->bytes))
                         .arg(result_->files));
    tree_->clear();
    auto* top = new QTreeWidgetItem(tree_);
    top->setText(0, root_);
    top->setText(1, bytes(result_->bytes));
    top->setText(3, QString::number(result_->files));
    top->setData(0, Qt::UserRole, QVariant::fromValue<void*>(result_.get()));
    addChildren(top, *result_, result_->bytes);
    top->setExpanded(true);
}

void DiskSpacePage::addChildren(QTreeWidgetItem* parent, const DirNode& node, double total) {
    std::vector<const DirNode*> kids;
    for (const auto& k : node.kids) kids.push_back(k.get());
    std::sort(kids.begin(), kids.end(), [](auto a, auto b) { return a->bytes > b->bytes; });
    double inKids = 0;
    for (const auto* k : kids) {
        inKids += k->bytes;
        if (k->bytes < 1e6 && kids.size() > 30) continue;  // hide crumbs in busy folders
        auto* it = new QTreeWidgetItem(parent);
        it->setText(0, QString::fromStdString(k->name));
        it->setIcon(0, QIcon::fromTheme("folder"));
        it->setText(1, bytes(k->bytes));
        double share = total > 0 ? k->bytes / total : 0;
        it->setText(2, QString("%1  %2").arg(QString(int(std::round(share * 12)), QChar(0x2588)), pct(share * 100, 0)));
        it->setForeground(2, pal().disk);
        it->setText(3, QString::number(k->files));
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it->setData(0, Qt::UserRole, QVariant::fromValue<void*>(const_cast<DirNode*>(k)));
        if (!k->kids.empty()) new QTreeWidgetItem(it);  // placeholder until opened
    }
    if (node.bytes - inKids > 0) {
        auto* files = new QTreeWidgetItem(parent);
        files->setText(0, "Files directly in this folder");
        files->setText(1, bytes(node.bytes - inKids));
        files->setForeground(0, pal().dim);
        files->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
}
