#include "connectionspage.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QVBoxLayout>
#include <arpa/inet.h>
#include <unistd.h>

#include <unordered_map>

#include "../format.h"
#include "../live.h"
#include "common.h"

namespace {

// /proc/net addresses are hex in network byte order: "0100007F:0035".
QString address(const QString& hex, bool v6) {
    QString ip = hex.section(':', 0, 0), port = hex.section(':', 1, 1);
    int p = port.toInt(nullptr, 16);
    QHostAddress a;
    if (!v6) {
        quint32 raw = ip.toUInt(nullptr, 16);
        a.setAddress(ntohl(raw));
    } else {
        Q_IPV6ADDR bytes{};
        for (int w = 0; w < 4; ++w) {
            quint32 word = ip.mid(w * 8, 8).toUInt(nullptr, 16);  // each 32-bit word is little-endian
            for (int b = 0; b < 4; ++b) bytes[w * 4 + b] = quint8((word >> (8 * b)) & 0xff);
        }
        a.setAddress(bytes);
        bool ok = false;
        QHostAddress v4(a.toIPv4Address(&ok));
        if (ok) a = v4;
    }
    QString text = a.toString();
    if (text.contains(':')) text = "[" + text + "]";
    return p ? text + ":" + QString::number(p) : text + ":*";
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
        QFile f(QString("/proc/net/") + proto);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const bool v6 = QString(proto).endsWith('6'), tcp = QString(proto).startsWith("tcp");
        bool header = true;
        for (const auto& line : QString::fromUtf8(f.readAll()).split('\n')) {
            if (header) {
                header = false;
                continue;
            }
            auto f2 = line.simplified().split(' ');
            if (f2.size() < 10) continue;
            int state = f2[3].toInt(nullptr, 16);
            QString remote = address(f2[2], v6);
            bool listen = tcp ? state == 10 : remote.endsWith(":*");
            ++total;
            if (listen && !listening_->isChecked()) continue;
            ++shown;
            auto o = owner.find(f2[9]);
            auto* it = new NumItem(tree_);
            it->setText(0, o != owner.end() ? o->second.second : (f2[7].toInt() == me ? "closed or system socket" : "another account"));
            if (o != owner.end()) {
                it->setText(1, QString::number(o->second.first));
                it->setData(1, Qt::UserRole, o->second.first);
            }
            it->setText(2, tcp ? "TCP" : "UDP");
            it->setText(3, address(f2[1], v6));
            it->setText(4, listen ? "" : remote);
            it->setText(5, tcp ? tcpState(state) : listen ? "listening" : "");
            it->setForeground(0, o != owner.end() ? pal().text : pal().dim);
            it->setIcon(0, iconPixmap("globe", listen ? pal().faint : pal().network, 18));
            if (keep == it->text(3) + it->text(4)) tree_->setCurrentItem(it);
        }
    }
    tree_->setSortingEnabled(true);
    applyFilter(tree_, filter_->text());
    count_->setText(QString("%1 connections shown of %2 open sockets.").arg(shown).arg(total));
}
