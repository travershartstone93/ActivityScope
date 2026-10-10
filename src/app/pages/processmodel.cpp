#include "processmodel.h"

#include <QFileInfo>
#include <functional>
#include <algorithm>
#include <unordered_set>

#include "../format.h"
#include "../theme.h"

void ProcessModel::setSnapshot(const as::SnapshotPtr& s) {
    snap_ = s;
    memTotal_ = s ? double(s->mem.total) : 0;
    rebuild();
}

void ProcessModel::setTree(bool tree) {
    tree_ = tree;
    rebuild();
}

void ProcessModel::setFilter(const QString& f) {
    filter_ = f.trimmed();
    rebuild();
}

void ProcessModel::setShowKernel(bool k) {
    kernel_ = k;
    rebuild();
}

void ProcessModel::sort(int column, Qt::SortOrder order) {
    sortCol_ = column;
    order_ = order;
    rebuild();
}

bool ProcessModel::matches(const as::ProcessInfo& p) const {
    if (filter_.isEmpty()) return true;
    return qs(p.name).contains(filter_, Qt::CaseInsensitive) || qs(p.user).contains(filter_, Qt::CaseInsensitive) ||
           QString::number(p.key.pid) == filter_ || qs(p.cmdline).contains(filter_, Qt::CaseInsensitive);
}

void ProcessModel::rebuild() {
    beginResetModel();
    nodes_.clear();
    nodes_.push_back(Node{});
    if (snap_) {
        const auto& procs = snap_->procs;
        std::unordered_map<int, int> nodeOfPid;
        std::vector<int> keep;
        for (int i = 0; i < int(procs.size()); ++i)
            if (kernel_ || !procs[i].kernel_thread) keep.push_back(i);

        std::unordered_set<int> visible;
        if (filter_.isEmpty()) {
            for (int i : keep) visible.insert(i);
        } else {
            std::unordered_map<int, int> idxOfPid;
            for (int i : keep) idxOfPid[procs[i].key.pid] = i;
            for (int i : keep) {
                if (!matches(procs[i])) continue;
                visible.insert(i);
                if (!tree_) continue;
                // In the tree, a match keeps its ancestors so it stays in context.
                int pp = procs[i].ppid;
                while (pp > 0) {
                    auto it = idxOfPid.find(pp);
                    if (it == idxOfPid.end() || !visible.insert(it->second).second) break;
                    pp = procs[it->second].ppid;
                }
            }
        }
        for (int i : keep) {
            if (!visible.count(i)) continue;
            nodeOfPid[procs[i].key.pid] = int(nodes_.size());
            Node n;
            n.proc = i;
            nodes_.push_back(n);
        }
        const int firstProc = 1;
        const int procEnd = int(nodes_.size());
        int sectionNode[SectionCount] = {-1, -1, -1, -1};
        if (tree_) {
            for (int sec = 0; sec < SectionCount; ++sec) {
                Node h;
                h.parent = 0;
                h.section = sec;
                sectionNode[sec] = int(nodes_.size());
                nodes_.push_back(h);
            }
        }
        for (int n = firstProc; n < procEnd; ++n) {
            const auto& p = procs[nodes_[n].proc];
            int parent = 0;
            if (tree_) {
                int sec = sectionOf(p);
                parent = sectionNode[sec];
                // Nest under the parent only within the same section, and never
                // under the service managers that start everything.
                auto it = nodeOfPid.find(p.ppid);
                if (it != nodeOfPid.end() && it->second != n) {
                    const auto& pp = procs[nodes_[it->second].proc];
                    bool manager = pp.key.pid == 1 || (pp.name == "systemd" && pp.cmdline.find("--user") != std::string::npos);
                    if (!manager && sectionOf(pp) == sec) parent = it->second;
                }
            }
            nodes_[n].parent = parent;
            nodes_[parent].children.push_back(n);
        }
        if (tree_) {
            // Drop empty sections, then put them in a fixed order.
            auto& top = nodes_[0].children;
            top.clear();
            for (int sec = 0; sec < SectionCount; ++sec) {
                bool skip = sec == Kernel && !kernel_;
                if (!nodes_[sectionNode[sec]].children.empty() && !skip) top.push_back(sectionNode[sec]);
            }
            for (int sec = 0; sec < SectionCount; ++sec) sortChildren(sectionNode[sec]);
        } else {
            sortChildren(0);
        }
    }
    endResetModel();
}

int ProcessModel::sectionOf(const as::ProcessInfo& p) {
    if (p.kernel_thread) return Kernel;
    const std::string& cg = p.cgroup;
    if (cg.find("/user@") != std::string::npos) {
        if (cg.find("/app.slice/") != std::string::npos && p.unit.rfind("app-", 0) == 0) return Apps;
        if (cg.find("/app.slice/") != std::string::npos && (p.unit.rfind("vte-spawn", 0) == 0 || p.unit.rfind("tmux", 0) == 0)) return Apps;
        return Services;
    }
    if (cg.find("/session-") != std::string::npos) return Apps;
    return System;
}

bool ProcessModel::isSection(const QModelIndex& i) const {
    if (!i.isValid()) return false;
    int n = int(i.internalId());
    return n > 0 && n < int(nodes_.size()) && nodes_[n].section >= 0;
}

int ProcessModel::sectionId(const QModelIndex& i) const {
    return isSection(i) ? nodes_[int(i.internalId())].section : -1;
}

QModelIndex ProcessModel::sectionIndex(int section) const {
    const auto& top = nodes_.empty() ? std::vector<int>{} : nodes_[0].children;
    for (int r = 0; r < int(top.size()); ++r)
        if (nodes_[top[r]].section == section) return createIndex(r, 0, quintptr(top[r]));
    return {};
}

void ProcessModel::sortChildren(int node) {
    auto& kids = nodes_[node].children;
    const auto& procs = snap_->procs;
    auto key = [&](int n) -> const as::ProcessInfo& { return procs[nodes_[n].proc]; };
    auto less = [&](int a, int b) {
        const auto &x = key(a), &y = key(b);
        switch (sortCol_) {
        case Name: return QString::compare(qs(x.name), qs(y.name), Qt::CaseInsensitive) < 0;
        case Pid: return x.key.pid < y.key.pid;
        case User: return x.user < y.user;
        case Status: return x.state < y.state;
        case Memory: return x.rss < y.rss;
        case Disk: return x.read_bps.or_else(0) + x.write_bps.or_else(0) < y.read_bps.or_else(0) + y.write_bps.or_else(0);
        case Gpu: return x.gpu_mem.or_else(0) < y.gpu_mem.or_else(0);
        default: return x.cpu + x.child_cpu < y.cpu + y.child_cpu;
        }
    };
    std::stable_sort(kids.begin(), kids.end(), [&](int a, int b) { return order_ == Qt::AscendingOrder ? less(a, b) : less(b, a); });
    for (int k : kids) sortChildren(k);
}

const as::ProcessInfo* ProcessModel::info(const QModelIndex& i) const {
    if (!i.isValid() || !snap_) return nullptr;
    int n = int(i.internalId());
    if (n <= 0 || n >= int(nodes_.size()) || nodes_[n].proc < 0) return nullptr;
    return &snap_->procs[nodes_[n].proc];
}

QModelIndex ProcessModel::indexOfPid(int pid) const {
    if (!snap_) return {};
    for (int n = 1; n < int(nodes_.size()); ++n) {
        if (nodes_[n].proc < 0 || snap_->procs[nodes_[n].proc].key.pid != pid) continue;
        const auto& sib = nodes_[nodes_[n].parent].children;
        int row = int(std::find(sib.begin(), sib.end(), n) - sib.begin());
        return createIndex(row, 0, quintptr(n));
    }
    return {};
}

QModelIndex ProcessModel::index(int row, int col, const QModelIndex& parent) const {
    int p = parent.isValid() ? int(parent.internalId()) : 0;
    if (p < 0 || p >= int(nodes_.size()) || row < 0 || row >= int(nodes_[p].children.size())) return {};
    return createIndex(row, col, quintptr(nodes_[p].children[row]));
}

QModelIndex ProcessModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    int n = int(child.internalId());
    int p = nodes_[n].parent;
    if (p <= 0) return {};
    const auto& sib = nodes_[nodes_[p].parent].children;
    int row = int(std::find(sib.begin(), sib.end(), p) - sib.begin());
    return createIndex(row, 0, quintptr(p));
}

int ProcessModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() && parent.column() != 0) return 0;
    int p = parent.isValid() ? int(parent.internalId()) : 0;
    return p < int(nodes_.size()) ? int(nodes_[p].children.size()) : 0;
}

QIcon ProcessModel::iconFor(const as::ProcessInfo& p) const {
    QString name = qs(p.name).toLower();
    auto it = icons_.constFind(name);
    if (it != icons_.constEnd()) return *it;
    QIcon icon;
    QString exeBase = QFileInfo(qs(p.exe)).fileName().toLower();
    for (const QString& candidate : {name, exeBase, QString("org.kde.") + name})
        if (!candidate.isEmpty() && QIcon::hasThemeIcon(candidate)) {
            icon = QIcon::fromTheme(candidate);
            break;
        }
    if (icon.isNull()) icon = QIcon::fromTheme(p.kernel_thread ? "cpu" : "application-x-executable");
    icons_.insert(name, icon);
    return icon;
}

QVariant ProcessModel::data(const QModelIndex& i, int role) const {
    if (isSection(i)) {
        const Node& n = nodes_[int(i.internalId())];
        static const char* names[] = {"Your apps", "Background services", "System", "Kernel threads"};
        // Count every process under the header, not only the top level.
        std::function<int(int)> count = [&](int k) {
            int c = 0;
            for (int ch : nodes_[k].children) c += 1 + count(ch);
            return c;
        };
        if (role == Qt::DisplayRole && i.column() == Name) return QString("%1 (%2)").arg(names[n.section]).arg(count(int(i.internalId())));
        if (role == Qt::DisplayRole && i.column() == Cpu) {
            double sum = 0;
            std::function<void(int)> add = [&](int k) {
                for (int ch : nodes_[k].children) {
                    const auto& q = snap_->procs[nodes_[ch].proc];
                    sum += q.cpu + q.child_cpu;
                    add(ch);
                }
            };
            add(int(i.internalId()));
            return pct(sum);
        }
        if (role == Qt::FontRole) {
            QFont f = Theme::get().body(15, QFont::Bold);
            return f;
        }
        if (role == Qt::TextAlignmentRole && i.column() >= Cpu) return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    const as::ProcessInfo* p = info(i);
    if (!p) return {};
    if (role == Qt::DisplayRole) {
        switch (i.column()) {
        case Name: return qs(p->name);
        case Pid: return p->key.pid;
        case User: return qs(p->user);
        case Status: {
            switch (p->state.empty() ? ' ' : p->state[0]) {
            case 'R': return "running";
            case 'S': return "sleeping";
            case 'D': return "waiting on disk";
            case 'Z': return "zombie";
            case 'T': return "stopped";
            case 'I': return "idle";
            default: return qs(p->state);
            }
        }
        case Cpu: return pct(p->cpu + p->child_cpu);
        case Memory: return bytes(double(p->rss));
        case Disk: {
            double b = p->read_bps.or_else(0) + p->write_bps.or_else(0);
            return b >= 1000 ? rate(b) : QString();
        }
        case Gpu: return p->gpu_mem.ok() ? bytes(p->gpu_mem.get()) : QString();
        }
    }
    if (role == Qt::UserRole) return p->cpu + p->child_cpu;  // for the bar
    if (role == Qt::DecorationRole && i.column() == Name) return iconFor(*p);
    if (role == Qt::TextAlignmentRole && i.column() >= Cpu) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ForegroundRole && p->self) return pal().dim;
    if (role == Qt::ToolTipRole) return qs(p->cmdline.empty() ? p->name : p->cmdline);
    return {};
}

QVariant ProcessModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal) return {};
    if (role == Qt::TextAlignmentRole) return int((section >= Cpu ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};
    static const char* names[] = {"Name", "PID", "User", "Status", "CPU", "Memory", "Disk", "GPU memory"};
    return names[section];
}
