#include "performancepage.h"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>

#include "../format.h"
#include "../live.h"
#include "../theme.h"
#include "../widgets/card.h"
#include "../widgets/historygraph.h"
#include "../widgets/segmentmeter.h"

namespace {

QLabel* statKey(const QString& t) {
    auto* l = new QLabel(t);
    l->setStyleSheet(QString("color:%1; font-size:13px;").arg(pal().dim.name()));
    return l;
}

QLabel* statVal(const QString& t) {
    auto* l = new QLabel(t);
    l->setStyleSheet("font-size:17px; font-weight:700;");
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

}  // namespace

PerformancePage::PerformancePage(QWidget* parent) : QWidget(parent), store_(std::make_unique<as::Store>(as::default_db_path())) {
    const Palette& P = pal();
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(22, 0, 22, 14);
    h->setSpacing(16);

    auto* listWrap = new QWidget;
    listBox_ = new QVBoxLayout(listWrap);
    listBox_->setContentsMargins(0, 0, 0, 0);
    listBox_->setSpacing(8);
    listBox_->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidget(listWrap);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedWidth(420);
    h->addWidget(scroll);

    auto* detail = new QWidget;
    auto* dv = new QVBoxLayout(detail);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(8);
    auto* head = new QHBoxLayout;
    title_ = new QLabel("CPU");
    title_->setFont(Theme::get().heading(32));
    range_ = new QComboBox;
    range_->addItems({"Live", "Last hour", "Last 6 hours", "Last 24 hours"});
    head->addWidget(title_);
    head->addStretch();
    head->addWidget(range_, 0, Qt::AlignTop);
    dv->addLayout(head);
    auto* meterRow = new QHBoxLayout;
    meter_ = new SegmentMeter(Qt::Horizontal);
    meter_->setFixedHeight(30);
    meter_->setSegmentSize(20);
    big_ = new QLabel;
    big_->setStyleSheet(QString("font-size:34px; font-weight:600; color:%1;").arg(P.gpu.lighter(120).name()));
    big_->setMinimumWidth(130);
    big_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    meterRow->addWidget(meter_, 1);
    meterRow->addWidget(big_);
    dv->addLayout(meterRow);
    sub_ = new QLabel;
    sub_->setStyleSheet(QString("font-size:18px; color:%1;").arg(P.dim.name()));
    dv->addWidget(sub_);
    caption_ = new QLabel;
    caption_->setStyleSheet(QString("font-size:13px; color:%1;").arg(P.dim.name()));
    dv->addWidget(caption_);

    coreGrid_ = new QWidget;
    coreLayout_ = new QGridLayout(coreGrid_);
    coreLayout_->setContentsMargins(0, 0, 0, 0);
    coreLayout_->setSpacing(8);
    dv->addWidget(coreGrid_, 3);
    graph_ = new HistoryGraph;
    graph_->setGrid(20, 8);
    dv->addWidget(graph_, 3);
    stats_ = new QGridLayout;
    stats_->setHorizontalSpacing(30);
    stats_->setVerticalSpacing(3);
    dv->addLayout(stats_);
    h->addWidget(detail, 1);

    connect(range_, &QComboBox::currentIndexChanged, this, [this] {
        pastLoaded_ = 0;
        refresh();
    });
}

void PerformancePage::ensureItems() {
    const auto& s = Live::get().now();
    if (!s) return;
    const Palette& P = pal();
    std::vector<std::tuple<QString, QString, QString, QColor>> want = {
        {"cpu", "cpu", "CPU", P.cpu}, {"mem", "memory-stick", "Memory", P.memory}};
    int gi = 0;
    for (const auto& g : s->gpus) want.push_back({"gpu:" + qs(g.pci), "monitor", QString("GPU %1").arg(gi++), P.gpu});
    for (const auto& d : s->disks) want.push_back({"disk:" + qs(d.name), "hard-drive", "Disk " + qs(d.name), P.disk});
    want.push_back({"net", "globe", "Network", P.network});
    want.push_back({"energy", "zap", "Energy", P.energy});
    want.push_back({"thermal", "thermometer", "Thermals", P.thermal});
    if (want.size() == items_.size()) return;

    for (auto& it : items_) it.card->deleteLater();
    items_.clear();
    for (const auto& [id, icon, title, color] : want) {
        Item it{id, icon, title, color};
        it.card = new Card(icon, title);
        it.card->setTitleSize(20);
        it.card->setClickable(true);
        it.card->setFixedHeight(104);
        auto* row = new QHBoxLayout;
        it.text = new QLabel;
        it.text->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
        it.text->setWordWrap(true);
        it.mini = new HistoryGraph;
        it.mini->setCompact(true);
        it.mini->setGrid(4, 3);
        it.mini->setFixedSize(110, 66);
        it.mini->setCapacity(Live::kCapacity);
        it.mini->setFrameColor(color);
        row->addWidget(it.text, 1);
        row->addWidget(it.mini);
        it.card->body()->setContentsMargins(52, 34, 10, 8);
        it.card->body()->addLayout(row);
        connect(it.card, &Card::clicked, this, [this, id = id] { select(id); });
        listBox_->insertWidget(listBox_->count() - 1, it.card);
        items_.push_back(it);
    }
    select(current_);
}

void PerformancePage::select(const QString& id) {
    current_ = id;
    for (auto& it : items_) it.card->setHighlighted(it.id == id);
    for (auto* g : coreGraphs_) g->deleteLater();
    coreGraphs_.clear();
    pastLoaded_ = 0;
    updateDetail();
}

void PerformancePage::refresh() {
    ensureItems();
    if (!isVisible()) return;
    updateList();
    updateDetail();
}

void PerformancePage::updateList() {
    const auto& s = Live::get().now();
    const Live& L = Live::get();
    if (!s) return;
    for (auto& it : items_) {
        QVector<GraphSeries> series;
        QString text;
        if (it.id == "cpu") {
            text = pct(s->cpu.total) + "\n" + QString("%1 logical processors").arg(s->cpu.logical);
            it.mini->setRange(0, 100);
            series = {{Live::series(L.kernel), pal().kernel, 1.2}, {Live::series(L.cpu), it.color, 1.5}};
        } else if (it.id == "mem") {
            text = bytes(double(s->mem.used)) + " / " + bytes(double(s->mem.total)) + "\n" + pct(s->mem.percent());
            it.mini->setRange(0, 100);
            series = {{Live::series(L.mem), it.color, 1.5, true}};
        } else if (it.id.startsWith("gpu:")) {
            for (const auto& g : s->gpus)
                if ("gpu:" + qs(g.pci) == it.id) {
                    text = qs(g.name) + "\n" + (g.asleep ? QString("Asleep") : g.util.ok() ? pct(g.util.get()) : QString("Load not readable"));
                    auto f = L.gpu.find(g.pci);
                    if (f != L.gpu.end()) series = {{Live::series(f->second), it.color, 1.5}};
                }
            it.mini->setRange(0, 100);
        } else if (it.id.startsWith("disk:")) {
            for (const auto& d : s->disks)
                if ("disk:" + qs(d.name) == it.id) {
                    text = qs(d.kind) + ", " + bytes(double(d.size)) + "\n" + pct(d.busy) + " busy";
                    auto f = L.disk.find(d.name);
                    if (f != L.disk.end()) series = {{Live::series(f->second), it.color, 1.5, true}};
                }
            it.mini->setAutoRange(1e6);
        } else if (it.id == "net") {
            double rx = L.netRx.last(), tx = L.netTx.last();
            text = "In " + rate(std::isnan(rx) ? 0 : rx) + ", out " + rate(std::isnan(tx) ? 0 : tx);
            it.mini->setAutoRange(64e3);
            series = {{Live::series(L.netTx), pal().temp, 1.2}, {Live::series(L.netRx), it.color, 1.5}};
        } else if (it.id == "energy") {
            const auto& pw = s->power;
            text = pw.battery_w.ok() ? QString::number(pw.battery_w.get(), 'f', 1) + " W, " : QString();
            text += pw.on_ac.ok() ? (pw.on_ac.get() ? "on AC power" : "on battery") : "";
            if (pw.battery_pct.ok()) text += QString("\nBattery %1%").arg(pw.battery_pct.get(), 0, 'f', 0);
            it.mini->setAutoRange(10);
            series = {{Live::series(L.power), it.color, 1.5}};
        } else if (it.id == "thermal") {
            text = s->cpu.temp_c.ok() ? "CPU " + celsius(s->cpu.temp_c.get()) : QString("CPU temperature unavailable");
            if (!Live::series(L.fan).isEmpty() && !std::isnan(L.fan.last())) text += "\nFan " + pct(L.fan.last(), 0);
            it.mini->setRange(0, 110);
            series = {{Live::series(L.fan), pal().fan, 1.2}, {Live::series(L.temp), it.color, 1.5}};
        }
        it.text->setText(text);
        it.mini->setSeries(series);
    }
}

void PerformancePage::setStats(const QList<QPair<QString, QString>>& rows) {
    while (QLayoutItem* it = stats_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const int perCol = (rows.size() + 3) / 4;
    for (int i = 0; i < rows.size(); ++i) {
        int col = i / std::max(1, perCol), row = i % std::max(1, perCol);
        stats_->addWidget(statKey(rows[i].first), row * 2, col);
        stats_->addWidget(statVal(rows[i].second), row * 2 + 1, col);
    }
}

void PerformancePage::loadPast() {
    const double now = QDateTime::currentSecsSinceEpoch();
    if (now - pastLoaded_ < 20) return;
    const double spans[] = {0, 3600, 6 * 3600, 24 * 3600};
    double from = now - spans[range_->currentIndex()];
    past_ = store_->samples(from, now);
    pastEvents_ = store_->events(from, now);
    pastLoaded_ = now;
}

void PerformancePage::updateDetail() {
    const auto& s = Live::get().now();
    const Live& L = Live::get();
    if (!s) return;
    const Palette& P = pal();
    const bool live = range_->currentIndex() == 0;
    Item* item = nullptr;
    for (auto& it : items_)
        if (it.id == current_) item = &it;
    if (!item) return;
    title_->setText(item->title);
    meter_->setColor(item->color);
    meter_->setHeat(current_ == "cpu");
    graph_->setFrameColor(item->color);
    graph_->setSpans({});
    coreGrid_->setVisible(live && current_ == "cpu" && !L.cores.empty());
    graph_->setVisible(!coreGrid_->isVisible());

    // Past ranges come from the recorded history, one value per sample.
    auto pastSeries = [&](auto get) {
        QVector<double> v;
        for (const auto& x : past_) v << get(x);
        return v;
    };
    if (!live) {
        loadPast();
        graph_->setCapacity(std::max(2, int(past_.size())));
        QVector<QPair<double, double>> spans;
        if (!past_.empty()) {
            double t0 = past_.front().t, t1 = past_.back().t, n = double(past_.size() - 1);
            for (const auto& e : pastEvents_)
                spans.push_back({(e.start - t0) / std::max(1.0, t1 - t0) * n, (e.end - t0) / std::max(1.0, t1 - t0) * n});
        }
        graph_->setSpans(spans);
        caption_->setText(past_.empty() ? "Nothing recorded for this range yet." : "Recorded history. Shaded areas are events; see the History page for what caused them.");
    } else {
        graph_->setCapacity(Live::kCapacity);
    }

    QList<QPair<QString, QString>> st;
    if (current_ == "cpu") {
        const auto& c = s->cpu;
        meter_->setValue(c.total / 100);
        big_->setText(pct(c.total));
        sub_->setText(qs(c.model));
        if (live) {
            caption_->setText("% Utilization by logical processor, last two minutes");
            if (int(coreGraphs_.size()) != int(L.cores.size())) {
                for (auto* g : coreGraphs_) g->deleteLater();
                coreGraphs_.clear();
                int cols = L.cores.size() > 16 ? 8 : L.cores.size() > 8 ? 6 : 4;
                for (int i = 0; i < int(L.cores.size()); ++i) {
                    auto* g = new HistoryGraph;
                    g->setCompact(true);
                    g->setGrid(4, 4);
                    g->setRange(0, 100);
                    g->setCapacity(Live::kCapacity);
                    g->setFrameColor(P.cpu);
                    g->setCornerText(QString("CPU %1").arg(i));
                    coreLayout_->addWidget(g, i / cols, i % cols);
                    coreGraphs_.push_back(g);
                }
            }
            for (int i = 0; i < int(coreGraphs_.size()); ++i) coreGraphs_[i]->setSeries({{Live::series(L.cores[i]), P.cpu, 1.6, true}});
        } else {
            graph_->setRange(0, 100);
            graph_->setAxisText("100%", "50%", "0%");
            graph_->setSeries({{pastSeries([](auto& x) { return x.kernel; }), P.kernel, 1.2}, {pastSeries([](auto& x) { return x.cpu; }), P.cpu, 1.6}});
        }
        st = {{"Utilization", pct(c.total)}, {"Kernel", pct(c.kernel)},
              {"Speed", orWhy(c.freq_mhz, mhz)}, {"Maximum speed", orWhy(c.max_mhz, mhz)},
              {"Base speed", orWhy(c.base_mhz, mhz)}, {"Temperature", orWhy(c.temp_c, celsius)},
              {"Processes", QString::number(s->procs.size())}, {"Threads", QString::number(s->threads_total)},
              {"Up time", duration(s->uptime)}, {"Load average", QString("%1  %2  %3").arg(c.load1).arg(c.load5).arg(c.load15)},
              {"Cores", QString("%1 physical, %2 logical").arg(c.physical).arg(c.logical)}, {"Sockets", QString::number(c.sockets)},
              {"L1 cache", c.cache_l1.ok() ? qs(c.cache_l1.get()) : "Unavailable"}, {"L2 cache", c.cache_l2.ok() ? qs(c.cache_l2.get()) : "Unavailable"},
              {"L3 cache", c.cache_l3.ok() ? qs(c.cache_l3.get()) : "Unavailable"}, {"Frequency driver", qs(c.driver)},
              {"Governor", qs(c.governor)}, {"Power preference", c.epp.empty() ? "Unavailable" : qs(c.epp)},
              {"Virtualization", c.virtualization ? "Supported" : "Not supported"}, {"Virtual machine", c.hypervisor ? "Yes" : "No"}};
    } else if (current_ == "mem") {
        const auto& m = s->mem;
        meter_->setValue(m.percent() / 100);
        big_->setText(pct(m.percent()));
        sub_->setText(bytes(double(m.total)) + " installed");
        graph_->setRange(0, 100);
        graph_->setAxisText("100%", "50%", "0%");
        if (live) {
            caption_->setText("Memory in use, last two minutes");
            graph_->setSeries({{Live::series(L.mem), P.memory, 2.0, true}});
        } else {
            graph_->setSeries({{pastSeries([](auto& x) { return x.mem_pct; }), P.memory, 1.6, true}});
        }
        uint64_t swapUsed = m.swap_total - std::min(m.swap_total, m.swap_free);
        st = {{"In use", bytes(double(m.used))}, {"Available", bytes(double(m.available))}, {"Cached", bytes(double(m.cached))},
              {"Buffers", bytes(double(m.buffers))}, {"Shared", bytes(double(m.shared))}, {"Waiting to be written", bytes(double(m.dirty))},
              {"Committed", bytes(double(m.committed)) + " of " + bytes(double(m.commit_limit))},
              {"Swap used", m.swap_total ? bytes(double(swapUsed)) + " of " + bytes(double(m.swap_total)) : "No swap"},
              {"Compressed in RAM", m.zram_orig ? bytes(double(m.zram_orig)) + " held in " + bytes(double(m.zram_compr)) : "Not in use"},
              {"Memory pressure", m.pressure_some10.ok() ? pct(m.pressure_some10.get()) + " of time waiting" : "Unavailable"},
              {"Swapped in (total)", bytes(double(m.page_ins) * 4096)}, {"Swapped out (total)", bytes(double(m.page_outs) * 4096)}};
    } else if (current_.startsWith("gpu:")) {
        for (const auto& g : s->gpus) {
            if ("gpu:" + qs(g.pci) != current_) continue;
            meter_->setValue(g.util.ok() ? g.util.get() / 100 : -1);
            big_->setText(g.util.ok() ? pct(g.util.get()) : g.asleep ? "Asleep" : "n/a");
            sub_->setText(qs(g.name));
            graph_->setRange(0, 100);
            graph_->setAxisText("100%", "50%", "0%");
            if (live) {
                caption_->setText(g.util.ok() ? "GPU load, last two minutes" : qs(g.util.why));
                auto f = L.gpu.find(g.pci);
                QVector<GraphSeries> series;
                if (f != L.gpu.end()) series.push_back({Live::series(f->second), P.gpu, 2.0, true});
                graph_->setSeries(series);
            } else {
                graph_->setSeries({{pastSeries([](auto& x) { return x.gpu >= 0 ? x.gpu : std::nan(""); }), P.gpu, 1.6, true}});
            }
            auto r = [](const as::Reading<double>& x, auto f) { return x.ok() ? f(x.get()) : QString("Unavailable"); };
            st = {{"Load", r(g.util, [](double v) { return pct(v); })}, {"Video memory used", r(g.mem_used, bytes)},
                  {"Video memory total", r(g.mem_total, bytes)}, {"Temperature", r(g.temp_c, celsius)},
                  {"Power draw", r(g.power_w, [](double v) { return QString::number(v, 'f', 1) + " W"; })},
                  {"Clock", r(g.clock_mhz, mhz)}, {"Driver", qs(g.driver)}, {"PCI address", qs(g.pci)},
                  {"Power state", g.asleep ? "Asleep (not woken to measure)" : "Awake"}};
        }
    } else if (current_.startsWith("disk:")) {
        for (const auto& d : s->disks) {
            if ("disk:" + qs(d.name) != current_) continue;
            meter_->setValue(d.busy / 100);
            big_->setText(pct(d.busy));
            sub_->setText(qs(d.model.empty() ? d.name : d.model) + " (" + qs(d.kind) + ")");
            graph_->setAutoRange(1e6);
            graph_->setAxisText("", "", "");
            if (live) {
                caption_->setText("Bytes moved per second, last two minutes");
                auto f = L.disk.find(d.name);
                QVector<GraphSeries> series;
                if (f != L.disk.end()) series.push_back({Live::series(f->second), P.disk, 2.0, true});
                graph_->setSeries(series);
            } else {
                graph_->setSeries({{pastSeries([](auto& x) { return x.disk_r + x.disk_w; }), P.disk, 1.6, true}});
                caption_->setText("All disks combined, recorded history");
            }
            graph_->setCornerText(bytes(graph_->max()) + "/s");
            st = {{"Busy", pct(d.busy)}, {"Reading", rate(d.read_bps)}, {"Writing", rate(d.write_bps)},
                  {"Capacity", bytes(double(d.size))}, {"Type", qs(d.kind)}, {"Temperature", orWhy(d.temp_c, celsius)},
                  {"Device", "/dev/" + qs(d.name)}};
        }
    } else if (current_ == "net") {
        double rx = 0, tx = 0;
        for (const auto& n : s->nets)
            if (n.kind != "Loopback" && n.kind != "Virtual") rx += n.rx_bps, tx += n.tx_bps;
        double peak = std::max(1.0, L.netRx.max() + L.netTx.max());
        meter_->setValue((rx + tx) / peak);
        big_->setText(rate(rx + tx));
        int up = 0;
        for (const auto& n : s->nets) up += n.up && n.kind != "Loopback" && n.kind != "Virtual";
        sub_->setText(QString("%1 connected interface%2").arg(up).arg(up == 1 ? "" : "s"));
        graph_->setAutoRange(64e3);
        graph_->setAxisText("", "", "");
        if (live) {
            caption_->setText("Received (blue) and sent (amber), last two minutes");
            graph_->setSeries({{Live::series(L.netTx), P.temp, 1.6}, {Live::series(L.netRx), P.network, 2.0, true}});
        } else {
            graph_->setSeries({{pastSeries([](auto& x) { return x.net_tx; }), P.temp, 1.2}, {pastSeries([](auto& x) { return x.net_rx; }), P.network, 1.6, true}});
        }
        graph_->setCornerText(bytes(graph_->max()) + "/s");
        for (const auto& n : s->nets) {
            if (n.kind == "Loopback" || (n.kind == "Virtual" && !n.up)) continue;
            QString v = QString("%1, %2. In %3, out %4").arg(qs(n.kind), n.up ? "connected" : "not connected", rate(n.rx_bps), rate(n.tx_bps));
            if (!n.ipv4.empty()) v += ". Address " + qs(n.ipv4);
            st << QPair<QString, QString>{qs(n.name), v};
        }
    } else if (current_ == "energy") {
        const auto& pw = s->power;
        meter_->setValue(pw.battery_pct.ok() ? pw.battery_pct.get() / 100 : -1);
        big_->setText(pw.battery_w.ok() ? QString::number(pw.battery_w.get(), 'f', 1) + " W" : "n/a");
        sub_->setText(pw.on_ac.ok() ? (pw.on_ac.get() ? "On AC power" : "On battery") : "Power source unknown");
        graph_->setAutoRange(10);
        graph_->setAxisText("", "", "");
        if (live) {
            caption_->setText("Battery draw in watts (negative while charging), last two minutes");
            graph_->setSeries({{Live::series(L.power), P.energy, 2.0, true}});
        } else {
            graph_->setSeries({{pastSeries([](auto& x) { return x.power >= 0 ? x.power : std::nan(""); }), P.energy, 1.6, true}});
        }
        graph_->setCornerText(QString::number(graph_->max(), 'f', 0) + " W");
        st = {{"Battery", pw.battery_pct.ok() ? pct(pw.battery_pct.get(), 0) : "No battery"}, {"State", qs(pw.battery_state)},
              {"Draw", pw.battery_w.ok() ? QString::number(pw.battery_w.get(), 'f', 1) + " W" : "Unavailable"},
              {"Battery health", pw.battery_health.ok() ? pct(pw.battery_health.get(), 0) + " of design capacity" : "Unavailable"},
              {"CPU package power", pw.cpu_package_w.ok() ? QString::number(pw.cpu_package_w.get(), 'f', 1) + " W" : qs(pw.cpu_package_w.why)},
              {"Power profile", pw.profile.empty() ? "Not reported" : qs(pw.profile)}};
    } else if (current_ == "thermal") {
        double hottest = 0;
        for (const auto& t : s->thermals) hottest = std::max(hottest, t.celsius);
        const auto& g = s->guard;
        meter_->setValue(g.temp.ok() ? g.temp.get() / g.limit.get() : hottest / 105);
        meter_->setHeat(true);
        big_->setText(g.temp.ok() ? celsius(g.temp.get()) : celsius(hottest));
        sub_->setText(g.margin.ok() ? QString("%1 °C below the %2 °C shutdown point").arg(QString::number(std::max(0.0, g.margin.get()), 'f', 1),
                                                                                       QString::number(g.limit.get(), 'f', 0))
                                    : QString("%1 temperature sensors, %2 fans").arg(s->thermals.size()).arg(s->power.fans.size()));
        st << QPair<QString, QString>{"Shutdown sensor", g.zone.empty() ? "None reported" : qs(g.zone)}
           << QPair<QString, QString>{"Shuts down at", orWhy(g.limit, celsius)}
           << QPair<QString, QString>{"Room to spare", g.margin.ok() ? QString::number(std::max(0.0, g.margin.get()), 'f', 1) + " °C" : qs(g.margin.why)}
           << QPair<QString, QString>{"Changing by", g.rate.ok() ? QString("%1%2 °C per second").arg(g.rate.get() >= 0 ? "+" : "").arg(g.rate.get(), 0, 'f', 2) : qs(g.rate.why)}
           << QPair<QString, QString>{"Shutdown at this rate", g.seconds_left.ok() ? QString("in about %1 s").arg(int(g.seconds_left.get())) : qs(g.seconds_left.why)}
           << QPair<QString, QString>{"Fans not spinning", g.dead_fans.empty() ? "None" : qs(g.dead_fans.front())};
        graph_->setRange(0, 110);
        graph_->setAxisText("110°", "55°", "0°");
        if (live) {
            caption_->setText("CPU temperature (orange), shutdown sensor (red) and fan speed in % (cyan), last two minutes. "
                              "The machine shuts itself down when the red line reaches the top.");
            QVector<double> limitLine;
            for (int i = 0; i < int(L.guard.size()); ++i) limitLine << (s->guard.limit.ok() ? s->guard.limit.get() : std::nan(""));
            graph_->setSeries({{Live::series(L.fan), P.fan, 1.4}, {limitLine, withAlpha(P.alert, 140), 1.0},
                               {Live::series(L.temp), P.thermal, 1.8}, {Live::series(L.guard), P.alert, 2.0}});
        } else {
            graph_->setSeries({{pastSeries([](auto& x) { return x.fan >= 0 ? x.fan : std::nan(""); }), P.fan, 1.2},
                               {pastSeries([](auto& x) { return x.temp >= 0 ? x.temp : std::nan(""); }), P.thermal, 1.6}});
        }
        for (const auto& t : s->thermals) {
            QString v = celsius(t.celsius) + (t.critical.ok() ? "  (limit " + celsius(t.critical.get()) + ")" : "");
            st << QPair<QString, QString>{chipName(t.chip) + ": " + qs(t.label), v};
        }
        for (const auto& f : s->power.fans)
            st << QPair<QString, QString>{qs(f.label), f.percent.ok() ? pct(f.percent.get(), 0) : f.rpm.ok() ? QString("%1 RPM").arg(f.rpm.get()) : "Unavailable"};
    }
    setStats(st);
}
