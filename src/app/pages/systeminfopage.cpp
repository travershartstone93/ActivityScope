#include "systeminfopage.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSysInfo>
#include <QVBoxLayout>
#include <sys/utsname.h>

#include "../format.h"
#include "../live.h"
#include "../widgets/card.h"
#include "common.h"
#include "store.h"

namespace {

QString sysfs(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

QString osName() {
    QFile f("/etc/os-release");
    if (f.open(QIODevice::ReadOnly))
        for (const auto& line : QString::fromUtf8(f.readAll()).split('\n'))
            if (line.startsWith("PRETTY_NAME=")) return line.mid(12).remove('"');
    return QSysInfo::prettyProductName();
}

}  // namespace

SystemInfoPage::SystemInfoPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 0, 22, 14);
    auto* top = new QHBoxLayout;
    top->addWidget(noteLabel("Everything here is read from the system as your own user."), 1);
    auto* copy = new QPushButton("Copy all");
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_); });
    top->addWidget(copy);
    outer->addLayout(top);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    grid_ = new QGridLayout(inner);
    grid_->setSpacing(14);
    grid_->setContentsMargins(0, 0, 8, 0);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);
}

void SystemInfoPage::refresh() {
    const auto& s = Live::get().now();
    if (!s || (built_ > 0 && s->wall - built_ < 30)) return;  // facts change slowly
    built_ = s->wall;
    while (QLayoutItem* it = grid_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    text_.clear();
    int n = 0;
    auto card = [&](const QString& icon, const QString& title, const QList<QPair<QString, QString>>& rows) {
        auto* c = new Card(icon, title);
        c->setTitleSize(20);
        auto* f = new QFormLayout;
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        f->setHorizontalSpacing(18);
        f->setVerticalSpacing(6);
        text_ += title + "\n";
        for (const auto& [k, v] : rows) {
            if (v.isEmpty()) continue;
            auto* key = new QLabel(k);
            key->setStyleSheet(QString("color:%1;").arg(pal().dim.name()));
            auto* val = new QLabel(v);
            val->setWordWrap(v.size() > 60);  // short values stay on one line
            val->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            f->addRow(key, val);
            text_ += "  " + k + ": " + v + "\n";
        }
        c->body()->addLayout(f);
        c->body()->addStretch();
        grid_->addWidget(c, n / 2, n % 2);
        ++n;
    };
    const QString dmi = "/sys/class/dmi/id/";
    card("info", "This computer", {{"Maker", sysfs(dmi + "sys_vendor")}, {"Model", sysfs(dmi + "product_name")},
                                   {"Board", sysfs(dmi + "board_name")},
                                   {"Firmware", sysfs(dmi + "bios_version") + "  (" + sysfs(dmi + "bios_date") + ")"},
                                   {"Name on the network", QSysInfo::machineHostName()}});
    utsname u{};
    uname(&u);
    card("server", "Operating system",
         {{"System", osName()}, {"Kernel", QString::fromUtf8(u.release)}, {"Architecture", QString::fromUtf8(u.machine)},
          {"Desktop", qEnvironmentVariable("XDG_CURRENT_DESKTOP") + " on " + qEnvironmentVariable("XDG_SESSION_TYPE")},
          {"Running since", QDateTime::fromSecsSinceEpoch(qint64(s->wall - s->uptime)).toString("ddd MMM d, h:mm AP") + "  (" + duration(s->uptime) + ")"}});
    const auto& c = s->cpu;
    card("cpu", "Processor", {{"Model", qs(c.model)}, {"Cores", QString("%1 physical, %2 logical").arg(c.physical).arg(c.logical)},
                              {"Speed", orWhy(c.base_mhz, mhz) + " base, " + orWhy(c.max_mhz, mhz) + " maximum"},
                              {"Cache", (c.cache_l2.ok() ? "L2 " + qs(c.cache_l2.get()) : QString()) + (c.cache_l3.ok() ? ", L3 " + qs(c.cache_l3.get()) : QString())},
                              {"Virtualization", c.virtualization ? "Supported" : "Not supported"}});
    const auto& m = s->mem;
    card("memory-stick", "Memory", {{"Installed", bytes(double(m.total))}, {"Swap", m.swap_total ? bytes(double(m.swap_total)) : "None"},
                                    {"Compressed swap in RAM", m.zram_orig ? "In use" : "Not in use"}});
    QList<QPair<QString, QString>> gfx;
    for (const auto& g : s->gpus) gfx << QPair<QString, QString>{qs(g.name), "Driver " + qs(g.driver) + ", " + qs(g.pci)};
    card("monitor", "Graphics", gfx);
    QList<QPair<QString, QString>> disks;
    for (const auto& d : s->disks) disks << QPair<QString, QString>{qs(d.name), qs(d.model) + ", " + qs(d.kind) + ", " + bytes(double(d.size))};
    card("hard-drive", "Storage", disks);
    QList<QPair<QString, QString>> screens;
    for (QScreen* sc : QGuiApplication::screens())
        screens << QPair<QString, QString>{sc->name(), QString("%1 × %2 at %3 Hz, scale %4")
                                                           .arg(sc->size().width() * sc->devicePixelRatio())
                                                           .arg(sc->size().height() * sc->devicePixelRatio())
                                                           .arg(sc->refreshRate(), 0, 'f', 0)
                                                           .arg(sc->devicePixelRatio())};
    card("layout-dashboard", "Displays", screens);
    QList<QPair<QString, QString>> nets;
    for (const auto& net : s->nets) {
        if (net.kind == "Loopback" || (net.kind == "Virtual" && !net.up)) continue;
        QString v = qs(net.kind) + (net.up ? ", connected" : ", not connected");
        if (!net.ipv4.empty()) v += ", " + qs(net.ipv4);
        if (net.speed_mbps.ok()) v += QString(", %1 Mb/s").arg(net.speed_mbps.get());
        nets << QPair<QString, QString>{qs(net.name), v};
    }
    card("globe", "Network", nets);
    const QString bat = "/sys/class/power_supply/";
    QString batName;
    for (const auto& e : QDir(bat).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (sysfs(bat + e + "/type") == "Battery") batName = e;
    if (!batName.isEmpty()) {
        const auto& p = s->power;
        QString cycles = sysfs(bat + batName + "/cycle_count");
        card("zap", "Battery", {{"Maker", sysfs(bat + batName + "/manufacturer")}, {"Model", sysfs(bat + batName + "/model_name")},
                                {"Health", p.battery_health.ok() ? pct(p.battery_health.get(), 0) + " of its original capacity" : "Not reported"},
                                {"Charge cycles", cycles.isEmpty() || cycles == "0" ? "Not reported" : cycles},
                                {"Chemistry", sysfs(bat + batName + "/technology")}});
    }
    QString db = qs(as::default_db_path());
    card("activity", "ActivityScope", {{"Version", QApplication::applicationVersion()}, {"Qt", qVersion()},
                                      {"History", db + "  (" + bytes(double(QFileInfo(db).size() + QFileInfo(db + "-wal").size())) + ")"}});
    grid_->setRowStretch(n / 2 + 1, 1);
}
