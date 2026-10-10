#include "powerpage.h"

#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>

#include "../format.h"
#include "../live.h"
#include "../widgets/card.h"
#include "../widgets/historygraph.h"
#include "common.h"

namespace {

QString sysfs(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

}  // namespace

// One column per logical processor: its current clock against the range the
// frequency driver allows.
class CoreBars : public QWidget {
public:
    explicit CoreBars(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(200); }
    void setData(std::vector<double> mhz, double lo, double hi) {
        mhz_ = std::move(mhz);
        lo_ = lo;
        hi_ = hi;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& P = pal();
        int n = int(mhz_.size());
        if (!n || hi_ <= 0) return;
        QRectF area(66, 6, width() - 72, height() - 30);
        p.setFont(Theme::get().body(11));
        p.setPen(P.dim);
        p.drawText(QRectF(0, area.top() - 6, 60, 14), Qt::AlignRight, mhz(hi_));
        p.drawText(QRectF(0, area.bottom() - 8, 60, 14), Qt::AlignRight, "0");
        // Dashed line at the lowest speed the driver allows.
        double yMin = area.bottom() - area.height() * lo_ / hi_;
        p.setPen(QPen(withAlpha(P.dim, 120), 1, Qt::DashLine));
        p.drawLine(QPointF(area.left(), yMin), QPointF(area.right(), yMin));
        p.setPen(P.dim);
        p.drawText(QRectF(0, yMin - 7, 60, 14), Qt::AlignRight, mhz(lo_));
        double w = area.width() / n;
        for (int i = 0; i < n; ++i) {
            QRectF slot(area.left() + i * w + w * 0.15, area.top(), w * 0.7, area.height());
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(P.clock, 30));
            p.drawRoundedRect(slot, 4, 4);
            double f = std::clamp(mhz_[i] / hi_, 0.0, 1.0);
            QRectF fill(slot.left(), slot.bottom() - slot.height() * f, slot.width(), slot.height() * f);
            p.setBrush(withAlpha(P.clock, 60));
            p.drawRoundedRect(fill.adjusted(-2, -2, 2, 0), 4, 4);
            p.setBrush(P.clock);
            p.drawRoundedRect(fill, 4, 4);
            p.setPen(P.dim);
            p.drawText(QRectF(slot.left() - 10, area.bottom() + 4, slot.width() + 20, 16), Qt::AlignCenter, QString::number(i));
        }
    }

private:
    std::vector<double> mhz_;
    double lo_ = 0, hi_ = 0;
};

PowerPage::PowerPage(QWidget* parent) : QWidget(parent) {
    const Palette& P = pal();
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(22, 0, 22, 14);
    grid->setSpacing(14);
    auto* cores = new Card("gauge", "Clock speed by processor");
    cores->setTitleSize(20);
    bars_ = new CoreBars;
    cores->body()->addWidget(bars_, 1);
    grid->addWidget(cores, 0, 0, 1, 2);
    auto* c1 = new Card("cpu", "Average clock");
    c1->setTitleSize(20);
    clock_ = new HistoryGraph;
    clock_->setCapacity(Live::kCapacity);
    clock_->setFrameColor(P.clock);
    c1->body()->addWidget(clock_, 1);
    grid->addWidget(c1, 1, 0);
    auto* c2 = new Card("zap", "Battery draw");
    c2->setTitleSize(20);
    draw_ = new HistoryGraph;
    draw_->setCapacity(Live::kCapacity);
    draw_->setFrameColor(P.energy);
    c2->body()->addWidget(draw_, 1);
    grid->addWidget(c2, 1, 1);
    auto* st = new Card("settings", "How the processor and battery are set up");
    st->setTitleSize(20);
    stats_ = new QGridLayout;
    stats_->setHorizontalSpacing(28);
    stats_->setVerticalSpacing(4);
    st->body()->addLayout(stats_);
    st->body()->addWidget(noteLabel("These are read-only here: changing them needs administrator rights, which ActivityScope never uses."));
    grid->addWidget(st, 2, 0, 1, 2);
    grid->setRowStretch(0, 3);
    grid->setRowStretch(1, 2);
}

void PowerPage::refresh() {
    const auto& s = Live::get().now();
    if (!s) return;
    const Live& L = Live::get();
    const Palette& P = pal();
    std::vector<double> mhzs;
    double lo = 1e9, hi = 0;
    for (int i = 0; i < s->cpu.logical; ++i) {
        QString base = QString("/sys/devices/system/cpu/cpu%1/cpufreq/").arg(i);
        mhzs.push_back(sysfs(base + "scaling_cur_freq").toDouble() / 1000);
        lo = std::min(lo, sysfs(base + "cpuinfo_min_freq").toDouble() / 1000);
        hi = std::max(hi, sysfs(base + "cpuinfo_max_freq").toDouble() / 1000);
    }
    bars_->setData(mhzs, lo, hi);
    clock_->setRange(0, hi > 0 ? hi : 5000);
    clock_->setAxisText(mhz(hi), "", "0");
    clock_->setSeries({{Live::series(L.freq), P.clock, 2.0, true}});
    draw_->setAutoRange(10);
    draw_->setSeries({{Live::series(L.power), P.energy, 2.0, true}});
    draw_->setCornerText(QString::number(draw_->max(), 'f', 0) + " W");

    while (QLayoutItem* it = stats_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QString cpu0 = "/sys/devices/system/cpu/cpu0/cpufreq/";
    QString noTurbo = sysfs("/sys/devices/system/cpu/intel_pstate/no_turbo");
    QString boost = sysfs("/sys/devices/system/cpu/cpufreq/boost");
    QString turbo = noTurbo == "0" || boost == "1" ? "On" : noTurbo == "1" || boost == "0" ? "Off" : "Not reported";
    const QString bat = "/sys/class/power_supply/BAT1/";
    const auto& pw = s->power;
    QString limit = sysfs(bat + "charge_control_end_threshold");
    QList<QPair<QString, QString>> rows = {
        {"Frequency driver", sysfs(cpu0 + "scaling_driver")}, {"Governor", sysfs(cpu0 + "scaling_governor")},
        {"Energy preference", sysfs(cpu0 + "energy_performance_preference")}, {"Turbo boost", turbo},
        {"Allowed range", mhz(sysfs(cpu0 + "scaling_min_freq").toDouble() / 1000) + " to " + mhz(sysfs(cpu0 + "scaling_max_freq").toDouble() / 1000)},
        {"Power profile", pw.profile.empty() ? "Not reported" : qs(pw.profile)},
        {"On", pw.on_ac.ok() ? (pw.on_ac.get() ? "AC power" : "Battery") : "Unknown"},
        {"Battery", pw.battery_pct.ok() ? pct(pw.battery_pct.get(), 0) + ", " + qs(pw.battery_state).toLower() : "None"},
        {"Draw", pw.battery_w.ok() ? QString::number(pw.battery_w.get(), 'f', 1) + " W" : "Unavailable"},
        {"Voltage", sysfs(bat + "voltage_now").isEmpty() ? "Unavailable" : QString::number(sysfs(bat + "voltage_now").toDouble() / 1e6, 'f', 2) + " V"},
        {"Charge limit", limit.isEmpty() || limit == "0" || limit == "100" ? "None" : limit + "%"},
        {"CPU package power", pw.cpu_package_w.ok() ? QString::number(pw.cpu_package_w.get(), 'f', 1) + " W" : qs(pw.cpu_package_w.why)}};
    for (int i = 0; i < rows.size(); ++i) {
        auto* k = noteLabel(rows[i].first);
        auto* v = new QLabel(rows[i].second);
        v->setStyleSheet("font-size:15px; font-weight:600;");
        v->setWordWrap(true);
        stats_->addWidget(k, (i / 4) * 2, i % 4);
        stats_->addWidget(v, (i / 4) * 2 + 1, i % 4);
    }
}
