#include "connectionspage.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <unistd.h>

#include <unordered_map>

#include "../format.h"
#include "../live.h"
#include "common.h"
#include "fs.h"
#include "netlog.h"

namespace {

// The decoding lives in the core (as::parse_proc_net); this only composes
// the display text.
QString address(const std::string& ip, int port) {
    QString text = QString::fromStdString(ip);
    if (text.contains(':')) text = "[" + text + "]";
    return port ? text + ":" + QString::number(port) : text + ":*";
}

const char* tcpState(int s) {
    static const char* names[] = {"", "connected", "opening", "opening", "closing", "closing", "closing", "closed",
                                  "closing", "closing", "listening", "closing"};
    return s > 0 && s < 12 ? names[s] : "";
}

}  // namespace

ConnectionsPage::ConnectionsPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    auto* bar = new QHBoxLayout;
    count_ = noteLabel();
    listening_ = new QCheckBox("Show programs waiting for connections");
    filter_ = filterBox("Filter by program, address or port");
    bar->addWidget(count_, 1);
    bar->addWidget(listening_);
    bar->addWidget(filter_);
    v->addLayout(bar);
    tree_ = styledTree({"Program", "PID", "Protocol", "This computer", "Other end", "State"});
    tree_->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 220);
    tree_->header()->resizeSection(1, 90);
    tree_->header()->resizeSection(2, 90);
    tree_->header()->resizeSection(3, 260);
    tree_->header()->resizeSection(5, 120);
    v->addWidget(tree_, 1);
    v->addWidget(noteLabel("Programs that belong to other accounts show as \"another account\": linking a connection to its "
                           "program needs to look inside that program, which only its owner or an administrator may do."));
    connect(listening_, &QCheckBox::toggled, this, [this] { reload(); });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) { applyFilter(tree_, f); });
}

void ConnectionsPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || (loaded_ > 0 && s->wall - loaded_ < 3)) return;
    loaded_ = s->wall;
    reload();
}

void ConnectionsPage::reload() {
    const auto& s = Live::get().now();
    std::unordered_map<QString, std::pair<int, QString>> owner;  // socket inode -> (pid, name)
    const int me = int(getuid());
    if (s)
        for (const auto& p : s->procs) {
            if (p.uid != me || p.kernel_thread) continue;
            QDir fd(QString("/proc/%1/fd").arg(p.key.pid));
            for (const auto& e : fd.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
                QString target = QFile::symLinkTarget(fd.filePath(e));
                if (target.contains("socket:[")) owner[target.section('[', 1).chopped(1)] = {p.key.pid, qs(p.name)};
            }
        }
    QString keep = tree_->currentItem() ? tree_->currentItem()->text(3) + tree_->currentItem()->text(4) : QString();
    tree_->setSortingEnabled(false);
    tree_->clear();
    int total = 0, shown = 0;
    for (const char* proto : {"tcp", "tcp6", "udp", "udp6"}) {
        auto text = as::read_file(QString("/proc/net/%1").arg(proto).toStdString(), 4 << 20);
        if (!text) continue;
        const bool tcp = QString(proto).startsWith("tcp");
        for (const auto& sock : as::parse_proc_net(*text, proto)) {
            bool listen = tcp ? sock.state == 10 : sock.remote_port == 0;
            ++total;
            if (listen && !listening_->isChecked()) continue;
            ++shown;
            auto o = owner.find(QString::number(sock.inode));
            auto* it = new NumItem(tree_);
            it->setText(0, o != owner.end() ? o->second.second : (sock.uid == me ? "closed or system socket" : "another account"));
            if (o != owner.end()) {
                it->setText(1, QString::number(o->second.first));
                it->setData(1, Qt::UserRole, o->second.first);
            }
            it->setText(2, tcp ? "TCP" : "UDP");
            it->setText(3, address(sock.local, sock.local_port));
            it->setText(4, listen ? "" : address(sock.remote, sock.remote_port));
            it->setText(5, tcp ? tcpState(sock.state) : listen ? "listening" : "");
            it->setForeground(0, o != owner.end() ? pal().text : pal().dim);
            it->setIcon(0, iconPixmap("globe", listen ? pal().faint : pal().network, 18));
            if (keep == it->text(3) + it->text(4)) tree_->setCurrentItem(it);
        }
    }
    tree_->setSortingEnabled(true);
    applyFilter(tree_, filter_->text());
    count_->setText(QString("%1 connections shown of %2 open sockets.").arg(shown).arg(total));
}
