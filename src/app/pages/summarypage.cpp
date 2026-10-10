#include "summarypage.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "../format.h"
#include "../live.h"
#include "../notifier.h"
#include "../theme.h"
#include "../widgets/card.h"
#include "../widgets/historygraph.h"
#include "../widgets/segmentmeter.h"

// The busiest processes, drawn as compact rows like an instrument readout.
class TopList : public QWidget {
public:
    explicit TopList(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(200); }
    void setRows(std::vector<const as::ProcessInfo*> rows, double memTotal) {
        rows_ = std::move(rows);
        memTotal_ = memTotal;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& P = pal();
        const int rowH = 25;
        QFont head = Theme::get().body(12, QFont::DemiBold);
        QFont body = Theme::get().body(13);
        const int wPid = 62, wCpu = 62, wMem = 86;
        auto cols = [&](int y, int h) {
            return std::array<QRect, 4>{QRect(0, y, wPid, h), QRect(wPid + 10, y, width() - wPid - wCpu - wMem - 20, h),
                                        QRect(width() - wCpu - wMem, y, wCpu, h), QRect(width() - wMem, y, wMem, h)};
        };
        p.setFont(head);
        p.setPen(P.dim);
        auto h = cols(0, 20);
        p.drawText(h[0], Qt::AlignRight | Qt::AlignVCenter, "PID");
        p.drawText(h[1], Qt::AlignLeft | Qt::AlignVCenter, "Name");
        p.drawText(h[2], Qt::AlignRight | Qt::AlignVCenter, "CPU");
        p.drawText(h[3], Qt::AlignRight | Qt::AlignVCenter, "Memory");
        p.setFont(body);
        int y = 26;
        for (const auto* r : rows_) {
            if (y + rowH > height()) break;
            if (r->cpu >= 1.0) {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(P.cpu, r->cpu >= 10 ? 70 : 34));
                p.drawRoundedRect(QRectF(-4, y, width() + 8, rowH - 3), 4, 4);
            }
            auto c = cols(y, rowH - 3);
            p.setPen(P.text);
            p.drawText(c[0], Qt::AlignRight | Qt::AlignVCenter, QString::number(r->key.pid));
            p.drawText(c[1], Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(body).elidedText(qs(r->name), Qt::ElideRight, c[1].width()));
            p.setPen(P.cpu.lighter(110));
            p.drawText(c[2], Qt::AlignRight | Qt::AlignVCenter, pct(r->cpu));
            p.setPen(P.text);
            p.drawText(c[3], Qt::AlignRight | Qt::AlignVCenter, bytes(double(r->rss)));
            y += rowH;
        }
    }

private:
    std::vector<const as::ProcessInfo*> rows_;
    double memTotal_ = 0;
};

namespace {

QLabel* small(const QString& text = {}, bool dim = true) {
    auto* l = new QLabel(text);
    l->setStyleSheet(QString("color:%1; font-size:13px;").arg((dim ? pal().dim : pal().text).name()));
    return l;
}

// One labelled vertical meter in the meters card.
QWidget* meterColumn(const QString& label, SegmentMeter*& meter, QLabel*& value, QColor color) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    auto* top = new QLabel(label);
    top->setAlignment(Qt::AlignCenter);
    top->setStyleSheet(QString("color:%1; font-size:12px; font-weight:600;").arg(color.name()));
    meter = new SegmentMeter(Qt::Vertical);
    meter->setColor(color);
    meter->setFixedWidth(44);
    value = new QLabel("-");
    value->setAlignment(Qt::AlignCenter);
    value->setStyleSheet(QString("color:%1; font-size:13px;").arg(color.name()));
    v->addWidget(top);
    v->addWidget(meter, 1, Qt::AlignHCenter);
    v->addWidget(value);
    return w;
}

Card* tile(const QString& icon, const QString& title, QColor tint, SegmentMeter*& bar) {
    auto* c = new Card(icon, title);
    c->setTint(tint);
    c->setTitleSize(17);
    c->setSubtitle(" ");
    bar = new SegmentMeter(Qt::Horizontal);
    bar->setColor(tint);
    bar->setFixedHeight(28);
    bar->setSegmentSize(10);
    c->body()->addStretch();
    c->body()->addWidget(bar);
    c->setMinimumHeight(118);
    return c;
}

}  // namespace

SummaryPage::SummaryPage(QWidget* parent) : QWidget(parent) {
    const Palette& P = pal();
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(22, 8, 22, 18);
    grid->setSpacing(14);

    banner_ = new Card("activity", "Recording an event");
    banner_->setTint(P.alert);
    banner_->setTitleSize(17);
    banner_->setClickable(true);
    bannerText_ = new QLabel;
    bannerText_->setWordWrap(true);
    bannerText_->setStyleSheet("font-size:14px;");
    banner_->body()->addWidget(bannerText_);
    banner_->hide();
    connect(banner_, &Card::clicked, this, &SummaryPage::openHistory);
    grid->addWidget(banner_, 0, 0, 1, 6);

    // Shown while the machine is close to its thermal shutdown, or while a
    // process is paused because of it.
    heat_ = new Card("thermometer", "Close to thermal shutdown");
    heat_->setTint(P.alert);
    heat_->setTitleSize(17);
    auto* hrow = new QHBoxLayout;
    heatText_ = new QLabel;
    heatText_->setWordWrap(true);
    heatText_->setStyleSheet("font-size:14px;");
    heatButton_ = new QPushButton;
    heatButton_->setObjectName("danger");
    heatButton_->setCursor(Qt::PointingHandCursor);
    hrow->addWidget(heatText_, 1);
    hrow->addWidget(heatButton_, 0, Qt::AlignVCenter);
    heat_->body()->addLayout(hrow);
    heat_->hide();
    connect(heatButton_, &QPushButton::clicked, this, [this] {
        Live& L = Live::get();
        QString err;
        bool isPaused = std::any_of(L.paused.begin(), L.paused.end(), [&](const auto& p) { return p.key == heatTarget_; });
        if (isPaused) L.resume(heatTarget_, &err);
        else if (heatTarget_.pid) L.pause(heatTarget_, heatName_, &err);
        if (!err.isEmpty()) heatText_->setText(err);
        refresh();
    });
    grid->addWidget(heat_, 0, 0, 1, 6);

    auto* meters = new Card;
    auto* mh = new QHBoxLayout;
    mh->setSpacing(10);
    mh->addWidget(meterColumn("CPU", mCpu_, lCpu_, P.cpu));
    mh->addWidget(meterColumn("Clock", mClock_, lClock_, P.clock));
    mh->addWidget(meterColumn("Temp", mTemp_, lTemp_, P.temp));
    mh->addWidget(meterColumn("GPU", mGpu_, lGpu_, P.gpu));
    meters->body()->addLayout(mh);
    grid->addWidget(meters, 1, 0, 1, 1);

    cpuCard_ = new Card("cpu", "CPU Overview");
    cpuCard_->setTitleSize(22);
    cpuCard_->setTint(QColor());
    auto* legend = new QLabel;
    legend->setText(QString("<span style='color:%1'>━ Utilization</span>&nbsp;&nbsp;&nbsp;<span style='color:%2'>━ Temperature</span>"
                            "&nbsp;&nbsp;&nbsp;<span style='color:%3'>━ Kernel</span>")
                        .arg(P.cpu.name(), P.temp.name(), P.kernel.name()));
    legend->setStyleSheet("font-size:13px;");
    cpuGraph_ = new HistoryGraph;
    cpuGraph_->setCapacity(Live::kCapacity);
    cpuGraph_->setAxisText("100%", "50%", "0%");
    cpuGraph_->setRightAxisText("110°", "55°", "0°");
    cpuGraph_->setFrameColor(P.cpu);
    auto* cf = new QHBoxLayout;
    cpuFootL_ = small();
    cpuFootR_ = small();
    cf->addWidget(cpuFootL_);
    cf->addStretch();
    cf->addWidget(cpuFootR_);
    cpuCard_->body()->addWidget(legend);
    cpuCard_->body()->addWidget(cpuGraph_, 1);
    cpuCard_->body()->addLayout(cf);
    grid->addWidget(cpuCard_, 1, 1, 1, 3);

    topCard_ = new Card("list-tree", "Top CPU processes");
    topCard_->setTitleSize(22);
    top_ = new TopList;
    topCard_->body()->addWidget(top_, 1);
    grid->addWidget(topCard_, 1, 4, 1, 2);

    memCard_ = new Card;
    auto* mrow = new QHBoxLayout;
    auto* mcol = new QVBoxLayout;
    mMem_ = new SegmentMeter(Qt::Vertical);
    mMem_->setColor(P.memory);
    mMem_->setFixedWidth(64);
    mMem_->setSegmentSize(6);
    lMem_ = new QLabel;
    lMem_->setAlignment(Qt::AlignCenter);
    lMem_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.memory.name()));
    mcol->addWidget(mMem_, 1);
    mcol->addWidget(lMem_);
    auto* mright = new QVBoxLayout;
    auto* mtitle = new QHBoxLayout;
    auto* mt = new QLabel("Memory Utilization");
    mt->setFont(Theme::get().heading(22));
    mtitle->addWidget(mt);
    mtitle->addStretch();
    memFootR_ = new QLabel;
    memFootR_->setStyleSheet("font-size:16px; font-weight:700;");
    mtitle->addWidget(memFootR_);
    memGraph_ = new HistoryGraph;
    memGraph_->setCapacity(Live::kCapacity);
    memGraph_->setFrameColor(P.memory);
    memGraph_->setGrid(24, 4);
    auto* mf = new QHBoxLayout;
    memFootL_ = small();
    memFootM_ = small();
    auto* swapLbl = small();
    swapLbl->setObjectName("swap");
    mf->addWidget(memFootL_);
    mf->addStretch();
    mf->addWidget(memFootM_);
    mf->addStretch();
    mf->addWidget(swapLbl);
    mright->addLayout(mtitle);
    mright->addWidget(memGraph_, 1);
    mright->addLayout(mf);
    mrow->addLayout(mcol);
    mrow->addSpacing(8);
    mrow->addLayout(mright, 1);
    memCard_->body()->addLayout(mrow);
    memCard_->setMinimumHeight(230);
    grid->addWidget(memCard_, 2, 0, 1, 6);

    disks_ = tile("hard-drive", "Disks", P.disk, bDisks_);
    net_ = tile("globe", "Network", P.network, bNet_);
    energy_ = tile("zap", "Energy", P.energy, bEnergy_);
    gpu_ = tile("monitor", "GPU", P.gpu, bGpu_);
    thermal_ = tile("thermometer", "Thermals", P.thermal, bThermal_);
    fan_ = tile("gauge", "Fan", P.fan, bFan_);
    grid->addWidget(disks_, 3, 0, 1, 2);
    grid->addWidget(net_, 3, 2, 1, 2);
    grid->addWidget(energy_, 3, 4, 1, 2);
    grid->addWidget(gpu_, 4, 0, 1, 2);
    grid->addWidget(thermal_, 4, 2, 1, 2);
    grid->addWidget(fan_, 4, 4, 1, 2);

    grid->setRowStretch(1, 5);
    grid->setRowStretch(2, 3);
    for (int c = 0; c < 6; ++c) grid->setColumnStretch(c, 1);
}

void SummaryPage::refresh() {
    const Live& L = Live::get();
    const auto& s = L.now();
    if (!s) return;
    const Palette& P = pal();

    const auto& g = s->guard;
    if (Notifier::danger(*s)) {
        bool pausable = false;
        const as::ProcessInfo* c = Notifier::heatCulprit(*s, &pausable);
        heat_->setTitle("Close to thermal shutdown");
        QString t = QString("%1 is at %2 °C; the system shuts itself down at %3 °C").arg(qs(g.zone), QString::number(g.temp.get(), 'f', 0),
                                                                                      QString::number(g.limit.get(), 'f', 0));
        t += g.seconds_left.ok() ? QString(", about %1 seconds away at this rate.").arg(int(g.seconds_left.get())) : QString(".");
        if (c) t += QString(" %1 is using %2 of the CPU.").arg(qs(c->name), pct(c->cpu, 0));
        heatText_->setText(t);
        heatTarget_ = c && pausable ? c->key : as::ProcessKey{};
        heatName_ = c ? qs(c->name) : QString();
        heatButton_->setText("Pause " + heatName_);
        heatButton_->setVisible(c && pausable);
        heat_->show();
    } else if (!L.paused.empty()) {
        const auto& p = L.paused.back();
        heat_->setTitle("Paused to cool down");
        heatText_->setText(QString("%1 (PID %2) has been paused since %3. %4 is now %5 °C.")
                               .arg(p.name).arg(p.key.pid).arg(clockText(p.when), qs(g.zone), g.temp.ok() ? QString::number(g.temp.get(), 'f', 0) : "-"));
        heatTarget_ = p.key;
        heatName_ = p.name;
        heatButton_->setText("Resume " + p.name);
        heatButton_->setVisible(true);
        heat_->show();
    } else {
        heat_->hide();
    }

    if (L.eventOpen && heat_->isHidden()) {
        banner_->show();
        if (L.openEvent) {
            banner_->setTitle("Recording an event: " + qs(as::kind_names(L.openEvent->kinds)));
            bannerText_->setText(qs(L.openEvent->summary));
        } else {
            banner_->setTitle("Recording an event");
            bannerText_->setText("Something is dragging the machine. ActivityScope is noting what is responsible.");
        }
    } else {
        banner_->hide();
    }

    mCpu_->setValue(s->cpu.total / 100);
    lCpu_->setText(pct(s->cpu.total));
    if (s->cpu.freq_mhz.ok() && s->cpu.max_mhz.ok()) {
        mClock_->setValue(s->cpu.freq_mhz.get() / s->cpu.max_mhz.get());
        lClock_->setText(mhz(s->cpu.freq_mhz.get()));
    } else {
        mClock_->setValue(-1);
        lClock_->setText("n/a");
    }
    if (s->cpu.temp_c.ok()) {
        mTemp_->setValue(s->cpu.temp_c.get() / 105.0);
        lTemp_->setText(celsius(s->cpu.temp_c.get()));
    } else {
        mTemp_->setValue(-1);
        lTemp_->setText("n/a");
    }
    double gutil = -1;
    QString gname, gsub;
    for (const auto& g : s->gpus) {
        if (g.vendor == "Intel" && s->gpus.size() > 1) continue;  // show the discrete card
        gname = qs(g.name);
        if (g.util.ok()) gutil = g.util.get();
        gsub = g.asleep ? "Asleep to save power" : g.util.ok() ? qs(g.name) : qs(g.util.why);
    }
    mGpu_->setValue(gutil >= 0 ? gutil / 100 : -1);
    lGpu_->setText(gutil >= 0 ? pct(gutil) : "idle");

    cpuCard_->setValue(pct(s->cpu.total));
    cpuGraph_->setRange(0, 100);
    QVector<double> temp = Live::series(L.temp);
    for (double& v : temp) v = v / 110.0 * 100.0;
    cpuGraph_->setSeries({{Live::series(L.kernel), P.kernel, 1.6, false},
                          {temp, P.temp, 1.8, false},
                          {Live::series(L.cpu), P.cpu, 2.0, false}});
    cpuFootL_->setText(QString("%1 logical processors").arg(s->cpu.logical));
    cpuFootR_->setText(s->cpu.freq_mhz.ok() ? "Speed " + mhz(s->cpu.freq_mhz.get()) : "Speed unavailable");

    std::vector<const as::ProcessInfo*> rows;
    for (const auto& p : s->procs)
        if (!p.kernel_thread) rows.push_back(&p);
    std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a->cpu > b->cpu; });
    top_->setRows(rows, double(s->mem.total));
    topCard_->setTitle(QString("Top CPU processes"));
    topCard_->setValue(QString("%1 total").arg(s->procs.size()));

    mMem_->setValue(s->mem.percent() / 100);
    lMem_->setText(pct(s->mem.percent()));
    memFootR_->setText(bytes(double(s->mem.used)) + " / " + bytes(double(s->mem.total)));
    memGraph_->setRange(0, 100);
    memGraph_->setSeries({{Live::series(L.mem), P.memory, 2.0, true}});
    memFootL_->setText("Available " + bytes(double(s->mem.available)));
    memFootM_->setText("Cached " + bytes(double(s->mem.cached)));
    if (auto* sw = findChild<QLabel*>("swap")) {
        uint64_t used = s->mem.swap_total - std::min(s->mem.swap_total, s->mem.swap_free);
        sw->setText(s->mem.swap_total ? "Swap " + bytes(double(used)) + " of " + bytes(double(s->mem.swap_total)) : "No swap");
    }

    double rd = 0, wr = 0, busy = 0;
    for (const auto& d : s->disks) rd += d.read_bps, wr += d.write_bps, busy = std::max(busy, d.busy);
    disks_->setValue(pct(busy));
    disks_->setSubtitle("Reading " + rate(rd) + ", writing " + rate(wr));
    bDisks_->setValue(busy / 100);

    double rx = 0, tx = 0;
    for (const auto& n : s->nets)
        if (n.kind != "Loopback" && n.kind != "Virtual") rx += n.rx_bps, tx += n.tx_bps;
    net_->setValue(rate(rx + tx));
    net_->setSubtitle("In " + rate(rx) + ", out " + rate(tx));
    double netMax = std::max(1e6, L.netRx.max() + L.netTx.max());
    bNet_->setValue((rx + tx) / netMax);

    const auto& pw = s->power;
    QString src = pw.on_ac.ok() ? (pw.on_ac.get() ? "AC power" : "On battery") : "";
    if (pw.battery_pct.ok()) src += QString(", battery at %1%").arg(pw.battery_pct.get(), 0, 'f', 0);
    energy_->setSubtitle(src);
    if (pw.battery_w.ok() && pw.battery_w.get() > 0.05) {
        energy_->setValue(QString::number(pw.battery_w.get(), 'f', 1) + " W");
        bEnergy_->setValue(pw.battery_w.get() / 90.0);
    } else if (pw.cpu_package_w.ok()) {
        energy_->setValue(QString::number(pw.cpu_package_w.get(), 'f', 1) + " W");
        bEnergy_->setValue(pw.cpu_package_w.get() / 90.0);
    } else {
        energy_->setValue(pw.battery_state == "Charging" ? "Charging" : "");
        bEnergy_->setValue(pw.battery_pct.ok() ? pw.battery_pct.get() / 100 : -1);
    }

    gpu_->setTitle(gname.isEmpty() ? "GPU" : gname.section(' ', 0, 0) + " GPU");
    gpu_->setSubtitle(gsub.isEmpty() ? gname : gsub);
    gpu_->setValue(gutil >= 0 ? pct(gutil) : "");
    bGpu_->setValue(gutil >= 0 ? gutil / 100 : -1);

    double hottest = -1;
    QString where;
    for (const auto& t : s->thermals)
        if (t.celsius > hottest) hottest = t.celsius, where = chipName(t.chip);
    if (g.margin.ok()) {
        // The tile is about how close the machine is to shutting itself down.
        thermal_->setValue(QString::number(g.temp.get(), 'f', 0) + " / " + QString::number(g.limit.get(), 'f', 0) + " °C");
        QString sub = QString("%1 °C below the shutdown point (%2)").arg(QString::number(std::max(0.0, g.margin.get()), 'f', 0), qs(g.zone));
        if (g.seconds_left.ok() && g.seconds_left.get() < 120) sub += QString(", about %1 s away at this rate").arg(int(g.seconds_left.get()));
        thermal_->setSubtitle(sub);
        thermal_->setTint(g.margin.get() <= 10 ? P.alert : P.thermal);
        bThermal_->setColor(g.margin.get() <= 10 ? P.alert : P.thermal);
        bThermal_->setValue(g.temp.get() / g.limit.get());
    } else {
        thermal_->setValue(hottest >= 0 ? celsius(hottest) : "");
        thermal_->setSubtitle(hottest >= 0 ? "Hottest of " + QString::number(s->thermals.size()) + " sensors: " + where : "No sensors");
        bThermal_->setValue(hottest >= 0 ? hottest / 105.0 : -1);
    }

    double fanPct = -1;
    QStringList fans;
    for (const auto& f : s->power.fans) {
        if (f.percent.ok()) fanPct = std::max(fanPct, double(f.percent.get())), fans << qs(f.label) + " " + pct(f.percent.get(), 0);
        else if (f.rpm.ok()) fans << qs(f.label) + QString(" %1 RPM").arg(f.rpm.get());
    }
    fan_->setValue(fanPct >= 0 ? pct(fanPct, 0) : "");
    fan_->setSubtitle(fans.isEmpty() ? "No fan sensor" : fans.join(", "));
    fan_->setTint(P.fan);
    if (!g.dead_fans.empty()) {
        fan_->setTitle("Fan not spinning");
        fan_->setSubtitle(QString("The %1 stays still while the machine is hot").arg(qs(g.dead_fans.front())));
        fan_->setTint(P.alert);
    } else {
        fan_->setTitle("Fan");
    }
    bFan_->setValue(fanPct >= 0 ? fanPct / 100 : -1);
}
