#include "historypage.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <cmath>

#include "../format.h"
#include "../icons.h"
#include "../live.h"
#include "../theme.h"
#include "../widgets/card.h"
#include "../widgets/historygraph.h"

namespace {

QString kindIcon(unsigned primary) {
    switch (primary) {
    case as::KindMemory: return "memory-stick";
    case as::KindDisk: return "hard-drive";
    case as::KindGpu: return "monitor";
    case as::KindHeat: return "thermometer";
    case as::KindShutdown: return "power";
    default: return "cpu";
    }
}

QColor kindColor(unsigned primary) {
    const Palette& P = pal();
    switch (primary) {
    case as::KindMemory: return P.memory;
    case as::KindDisk: return P.disk;
    case as::KindGpu: return P.gpu;
    case as::KindHeat: return P.thermal;
    case as::KindShutdown: return P.alert;
    default: return P.cpu;
    }
}

// "CPU", "Memory and heat", "CPU, memory and disk".
QString kindPhrase(unsigned kinds) {
    QStringList w;
    if (kinds & as::KindCpu) w << "CPU";
    if (kinds & as::KindMemory) w << "memory";
    if (kinds & as::KindDisk) w << "disk";
    if (kinds & as::KindGpu) w << "GPU";
    if (kinds & as::KindHeat) w << "heat";
    if (w.isEmpty()) return "Resource";
    QString s = w.size() == 1 ? w[0] : w.mid(0, w.size() - 1).join(", ") + " and " + w.last();
    if (s[0].isLower()) s[0] = s[0].toUpper();
    return s;
}

QString dayLabel(double t) {
    QDateTime d = QDateTime::fromSecsSinceEpoch(qint64(t));
    QDate today = QDate::currentDate();
    if (d.date() == today) return "Today";
    if (d.date() == today.addDays(-1)) return "Yesterday";
    return d.date().toString("ddd MMM d");
}

}  // namespace

// 24 hours (or less) of CPU and memory with every event marked; clicking a
// moment asks the page to explain it.
class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(150);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }
    void setData(std::vector<as::StoredSample> s, std::vector<as::EventRow> ev, double from, double to) {
        samples_ = std::move(s);
        events_ = std::move(ev);
        from_ = from;
        to_ = to;
        update();
    }
    void setSelected(double a, double b) {
        selA_ = a;
        selB_ = b;
        update();
    }

signals:
    void picked(double t);

protected:
    double tAt(double x) const { return from_ + (x - 50) / std::max(1.0, width() - 60.0) * (to_ - from_); }
    double xAt(double t) const { return 50 + (t - from_) / std::max(1.0, to_ - from_) * (width() - 60.0); }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->position().x() >= 50) emit picked(tAt(e->position().x()));
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        hover_ = e->position().x() >= 50 ? tAt(e->position().x()) : -1;
        update();
    }
    void leaveEvent(QEvent*) override {
        hover_ = -1;
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& P = pal();
        QRectF r(50, 8, width() - 60, height() - 34);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(P.cpu, 10));
        p.drawRoundedRect(r, 5, 5);
        p.setFont(Theme::get().body(11));
        // Hour grid and labels.
        double span = to_ - from_;
        double step = span > 12 * 3600 ? 3 * 3600 : span > 4 * 3600 ? 3600 : span > 3600 ? 900 : 300;
        double first = std::ceil(from_ / step) * step;
        for (double t = first; t < to_; t += step) {
            double x = xAt(t);
            p.setPen(QPen(P.grid, 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(P.dim);
            p.drawText(QRectF(x - 40, r.bottom() + 4, 80, 18), Qt::AlignCenter,
                       QDateTime::fromSecsSinceEpoch(qint64(t)).toString("h:mm AP"));
        }
        p.setPen(P.dim);
        p.drawText(QRectF(0, r.top(), 44, 16), Qt::AlignRight, "100%");
        p.drawText(QRectF(0, r.bottom() - 16, 44, 16), Qt::AlignRight, "0%");

        for (const auto& e : events_) {
            QRectF span(QPointF(xAt(e.start), r.top()), QPointF(std::max(xAt(e.end), xAt(e.start) + 3), r.bottom()));
            QColor c = kindColor(e.primary);
            bool sel = std::abs(e.start - selA_) < 0.5;
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(P.alert, sel ? 90 : 38));
            p.drawRect(span);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(span.left(), r.top() - 6, std::max(4.0, span.width()), 5), 2, 2);
        }
        if (selA_ > 0 && selB_ > selA_) {
            p.setPen(QPen(P.text, 1.2, Qt::DashLine));
            p.drawLine(QPointF(xAt(selA_), r.top()), QPointF(xAt(selA_), r.bottom()));
        }

        // Average into one value per pixel column so a day of seconds draws cleanly.
        int cols = int(r.width());
        auto line = [&](auto get, QColor c) {
            std::vector<double> sum(cols, 0), n(cols, 0);
            for (const auto& s : samples_) {
                int i = int((s.t - from_) / std::max(1.0, span) * cols);
                if (i < 0 || i >= cols) continue;
                sum[i] += get(s);
                n[i] += 1;
            }
            QPainterPath path;
            bool open = false;
            for (int i = 0; i < cols; ++i) {
                if (n[i] == 0) {
                    open = false;
                    continue;
                }
                QPointF pt(r.left() + i, r.bottom() - std::clamp(sum[i] / n[i] / 100.0, 0.0, 1.0) * r.height());
                if (!open) path.moveTo(pt), open = true;
                else path.lineTo(pt);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(withAlpha(c, 60), 4));
            p.drawPath(path);
            p.setPen(QPen(c, 1.4));
            p.drawPath(path);
        };
        line([](const as::StoredSample& s) { return s.mem_pct; }, P.memory);
        line([](const as::StoredSample& s) { return s.cpu; }, P.cpu);

        p.setPen(QPen(withAlpha(P.cpu, 90), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 5, 5);
        if (hover_ > 0) {
            p.setPen(QPen(withAlpha(P.text, 150), 1));
            p.drawLine(QPointF(xAt(hover_), r.top()), QPointF(xAt(hover_), r.bottom()));
            p.setPen(P.text);
            p.drawText(QRectF(xAt(hover_) + 6, r.top() + 4, 120, 16), Qt::AlignLeft,
                       QDateTime::fromSecsSinceEpoch(qint64(hover_)).toString("h:mm:ss AP"));
        }
    }

private:
    std::vector<as::StoredSample> samples_;
    std::vector<as::EventRow> events_;
    double from_ = 0, to_ = 1, hover_ = -1, selA_ = 0, selB_ = 0;
};

// Ranked culprits: name, what it is, and a bar for its share.
class CulpritList : public QWidget {
    Q_OBJECT
public:
    explicit CulpritList(QWidget* parent = nullptr) : QWidget(parent) { setMouseTracking(true); }
    void setCulprits(std::vector<as::Culprit> c, unsigned primary, bool live) {
        c_ = std::move(c);
        primary_ = primary;
        live_ = live;
        setMinimumHeight(int(c_.size()) * kRow + 8);
        update();
    }
    static constexpr int kRow = 62;

signals:
    void open(int pid);

protected:
    int rowAt(double y) const { return int(y / kRow); }
    void mousePressEvent(QMouseEvent* e) override {
        int i = rowAt(e->position().y());
        if (i >= 0 && i < int(c_.size()) && c_[i].kind == "process" && !c_[i].exited) emit open(c_[i].pid);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        int i = rowAt(e->position().y());
        bool link = i >= 0 && i < int(c_.size()) && c_[i].kind == "process" && !c_[i].exited;
        setCursor(link ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(link ? "Show this process in Processes" : QString());
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& P = pal();
        double top = 1;
        for (const auto& c : c_) top = std::max(top, metric(c));
        int y = 0;
        for (const auto& c : c_) {
            QRectF row(0, y + 3, width(), kRow - 6);
            p.setPen(Qt::NoPen);
            p.setBrush(P.panelHi);
            p.drawRoundedRect(row, 7, 7);
            QColor col = kindColor(primary_);
            double frac = std::clamp(metric(c) / top, 0.0, 1.0);
            p.setBrush(withAlpha(col, 46));
            p.drawRoundedRect(QRectF(row.left(), row.top(), row.width() * frac, row.height()), 7, 7);
            p.setBrush(col);
            p.drawRoundedRect(QRectF(row.left(), row.bottom() - 3, std::max(6.0, row.width() * frac), 3), 1.5, 1.5);

            QString title, sub;
            if (c.kind == "group") {
                title = QString("%1 short-lived %2").arg(c.count).arg(qs(c.name));
                sub = "Started by " + qs(c.parent) + (c.unit.empty() ? QString() : " in " + qs(c.unit));
            } else if (c.kind == "unseen") {
                title = "Programs too brief to catch, started by " + qs(c.name);
                sub = QString("Counted from the time %1 spent waiting on its children (PID %2)").arg(qs(c.name)).arg(c.pid);
            } else if (c.kind == "unit") {
                title = "Other work inside " + qs(c.name);
                sub = "Counted by the system for this service, from processes that came and went";
            } else {
                title = QString("%1  (PID %2)").arg(qs(c.name)).arg(c.pid);
                sub = qs(c.cmdline.empty() ? c.unit : c.cmdline);
                if (!c.parent.empty()) sub = "Started by " + qs(c.parent) + ": " + sub;
            }
            QString status = c.kind == "process" ? (c.exited ? "since exited" : live_ ? "running" : "") : "";
            p.setPen(P.text);
            p.setFont(Theme::get().body(15, QFont::DemiBold));
            QRectF tr = row.adjusted(14, 7, -200, -row.height() / 2);
            p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(title, Qt::ElideRight, int(tr.width())));
            p.setPen(P.dim);
            p.setFont(Theme::get().body(12));
            QRectF sr = row.adjusted(14, row.height() / 2, -200, -7);
            p.drawText(sr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(sub, Qt::ElideMiddle, int(sr.width())));
            p.setPen(col.lighter(115));
            p.setFont(Theme::get().body(15, QFont::Bold));
            QRectF vr = row.adjusted(row.width() - 190, 7, -14, -row.height() / 2);
            p.drawText(vr, Qt::AlignRight | Qt::AlignVCenter, value(c));
            p.setPen(P.dim);
            p.setFont(Theme::get().body(12));
            p.drawText(row.adjusted(row.width() - 190, row.height() / 2, -14, -7), Qt::AlignRight | Qt::AlignVCenter, status);
            y += kRow;
        }
    }

private:
    double metric(const as::Culprit& c) const {
        switch (primary_) {
        case as::KindMemory: return std::max(c.rss_growth > 50e6 ? double(c.rss_growth) : double(c.peak_rss) * 0.2, double(c.faults) * 4096 * 20);
        case as::KindDisk: return c.io_bytes;
        case as::KindGpu: return c.gpu_mem > 0 ? c.gpu_mem : c.cpu_seconds * 1e6;
        default: return c.cpu_seconds;
        }
    }
    QString value(const as::Culprit& c) const {
        switch (primary_) {
        case as::KindMemory:
            if (c.rss_growth <= 50e6 && c.faults >= 100) return QString("%1 swap waits").arg(c.faults);
            return c.rss_growth > 50e6 ? "+" + bytes(double(c.rss_growth)) : bytes(double(c.peak_rss));
        case as::KindDisk: return bytes(c.io_bytes);
        case as::KindGpu: return c.gpu_mem > 0 ? bytes(c.gpu_mem) + " VRAM" : pct(c.share * 100, 0) + " of CPU";
        default: return pct(c.share * 100, 0) + " of CPU";
        }
    }
    std::vector<as::Culprit> c_;
    unsigned primary_ = as::KindCpu;
    bool live_ = false;
};

HistoryPage::HistoryPage(QWidget* parent) : QWidget(parent), store_(std::make_unique<as::Store>(as::default_db_path())) {
    const Palette& P = pal();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(12);

    auto* tlCard = new Card("activity", "Timeline");
    tlCard->setTitleSize(20);
    auto* tlHead = new QHBoxLayout;
    auto* hint = new QLabel(QString("<span style='color:%1'>━ CPU</span>&nbsp;&nbsp;<span style='color:%2'>━ Memory</span>"
                                    "&nbsp;&nbsp;<span style='color:%3'>▮ Events</span>&nbsp;&nbsp;&nbsp;"
                                    "<span style='color:%4'>Click any moment to see what was running then.</span>")
                                .arg(P.cpu.name(), P.memory.name(), P.alert.name(), P.dim.name()));
    range_ = new QComboBox;
    range_->addItems({"Last hour", "Last 6 hours", "Last 24 hours"});
    range_->setCurrentIndex(0);
    tlHead->addWidget(hint, 1);
    tlHead->addWidget(range_);
    timeline_ = new Timeline;
    tlCard->body()->addLayout(tlHead);
    tlCard->body()->addWidget(timeline_);
    tlCard->setFixedHeight(250);
    v->addWidget(tlCard);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setHandleWidth(14);
    split->setStyleSheet("QSplitter::handle { background: transparent; }");
    auto* listCard = new Card("server", "Events");
    listCard->setTitleSize(20);
    list_ = new QListWidget;
    list_->setSpacing(3);
    list_->setStyleSheet(QString("QListWidget::item { border-radius:7px; padding:8px; margin:1px 0; background:%1; }"
                                 "QListWidget::item:selected { background:%2; }")
                             .arg(P.panelHi.name(QColor::HexArgb), P.selection.name(QColor::HexArgb)));
    list_->setWordWrap(true);
    listCard->body()->addWidget(list_);
    split->addWidget(listCard);

    detail_ = new Card("cpu", "Pick an event");
    detail_->setTitleSize(20);
    meta_ = new QLabel;
    meta_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
    summary_ = new QLabel("ActivityScope records every stretch of heavy CPU, memory, disk, GPU or heat, with the programs "
                          "behind it. Events appear here as they happen.");
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setStyleSheet("font-size:16px;");
    summary_->setMaximumWidth(780);  // about 80 characters a line
    triggers_ = new QLabel;
    triggers_->setWordWrap(true);
    triggers_->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
    graph_ = new HistoryGraph;
    graph_->setMinimumHeight(150);
    graph_->setMaximumHeight(190);
    graph_->setAxisText("100%", "50%", "0%");
    graphNote_ = new QLabel;
    graphNote_->setStyleSheet(QString("color:%1; font-size:12px;").arg(P.dim.name()));
    culprits_ = new CulpritList;
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(culprits_);
    auto* culHead = new QLabel("What was responsible");
    culHead->setStyleSheet("font-size:15px; font-weight:600;");
    detail_->body()->addWidget(meta_);
    detail_->body()->addWidget(summary_);
    detail_->body()->addWidget(triggers_);
    detail_->body()->addWidget(graph_);
    detail_->body()->addWidget(graphNote_);
    detail_->body()->addWidget(culHead);
    detail_->body()->addWidget(scroll, 1);
    split->addWidget(detail_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 5);
    split->setSizes({420, 1000});
    v->addWidget(split, 1);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        qint64 id = list_->item(row)->data(Qt::UserRole).toLongLong();
        if (id == -1) showOpen();
        else showEvent(id);
    });
    connect(timeline_, &Timeline::picked, this, [this](double t) {
        for (int i = 0; i < int(events_.size()); ++i)
            if (t >= events_[i].start - 30 && t <= events_[i].end + 30) {
                for (int r = 0; r < list_->count(); ++r)
                    if (list_->item(r)->data(Qt::UserRole).toLongLong() == events_[i].id) list_->setCurrentRow(r);
                return;
            }
        list_->clearSelection();
        showMoment(t);
    });
    connect(range_, &QComboBox::currentIndexChanged, this, [this] { loadTimeline(); });
    connect(culprits_, &CulpritList::open, this, &HistoryPage::showProcess);
    connect(&Live::get(), &Live::eventChanged, this, &HistoryPage::reloadEvents);
    reloadEvents();
}

void HistoryPage::reloadEvents() {
    const double now = QDateTime::currentSecsSinceEpoch();
    events_ = store_->events(now - 30 * 86400, now + 60);
    int64_t keep = current_;
    QSignalBlocker block(list_);
    list_->clear();
    const Palette& P = pal();
    auto addItem = [&](const QString& head, const QString& body, unsigned primary, qint64 id) {
        auto* it = new QListWidgetItem(QIcon(iconPixmap(kindIcon(primary), kindColor(primary), 22)), head + "\n" + body);
        it->setData(Qt::UserRole, id);
        it->setSizeHint(QSize(100, 64));
        list_->addItem(it);
        return it;
    };
    if (Live::get().eventOpen) {
        auto* it = addItem("Happening now", "Recording; the culprits update live", as::KindCpu, -1);
        it->setForeground(P.alert);
    }
    QString lastDay;
    for (const auto& e : events_) {
        QString day = dayLabel(e.start);
        QString first = qs(e.summary);
        int cut = first.indexOf(')');
        first = cut > 0 ? first.mid(cut + 2) : first;
        QString head = QString("%1 at %2, for %3").arg(day, clockText(e.start), duration(std::max(1.0, e.end - e.start)));
        auto* it = addItem(head, QFontMetrics(list_->font()).elidedText(first, Qt::ElideRight, 900), e.primary, e.id);
        it->setToolTip(qs(e.summary));
        lastDay = day;
    }
    if (events_.empty() && !Live::get().eventOpen) {
        auto* it = new QListWidgetItem("No events yet.\nWhen something drags the machine, it will appear here.");
        it->setFlags(Qt::NoItemFlags);
        list_->addItem(it);
    }
    bool kept = false;
    for (int r = 0; r < list_->count(); ++r)
        if (list_->item(r)->data(Qt::UserRole).toLongLong() == keep && keep) {
            list_->setCurrentRow(r);
            kept = true;
        }
    QDateTime midnight(QDate::currentDate(), QTime(0, 0));
    int today = 0;
    for (const auto& e : events_) today += e.start >= double(midnight.toSecsSinceEpoch());
    Live::get().eventsToday = today;
    loadTimeline();
    block.unblock();
    // Open on the newest event so the page answers "what just happened" at a glance.
    if (!kept && list_->count() > 0 && (list_->item(0)->flags() & Qt::ItemIsSelectable)) list_->setCurrentRow(0);
}

void HistoryPage::loadTimeline() {
    const double now = QDateTime::currentSecsSinceEpoch();
    const double spans[] = {3600, 6 * 3600, 24 * 3600};
    double from = now - spans[range_->currentIndex()];
    auto evs = store_->events(from, now);
    if (Live::get().eventOpen && Live::get().openEvent) {
        as::EventRow open;
        open.start = Live::get().openEvent->start;
        open.end = now;
        open.primary = Live::get().openEvent->primary;
        evs.push_back(open);
    }
    timeline_->setData(store_->samples(from, now), evs, from, now);
    lastTimeline_ = now;
}

void HistoryPage::refresh() {
    const double now = QDateTime::currentSecsSinceEpoch();
    if (now - lastTimeline_ >= 15) loadTimeline();
    if (showingOpen_) showOpen();
}

void HistoryPage::fill(const as::Event& e) {
    const Palette& P = pal();
    QColor c = kindColor(e.primary);
    detail_->setTitle(e.kinds & as::KindShutdown ? (e.kinds & as::KindHeat ? QString("Thermal shutdown") : QString("Unexpected power-off"))
                                                 : kindPhrase(e.kinds) + (e.open ? " event in progress" : " event"));
    detail_->setTint(QColor());
    QString when = QDateTime::fromSecsSinceEpoch(qint64(e.start)).toString("dddd MMM d");
    meta_->setText(QString("%1, %2 to %3 (%4)").arg(when, clockText(e.start), clockText(e.end), duration(std::max(1.0, e.end - e.start))));
    QString text = qs(e.summary);
    int cut = text.indexOf("). ");
    summary_->setText(cut > 0 ? text.mid(cut + 3) : text);
    QStringList t;
    for (const auto& s : e.triggers) t << qs(s);
    triggers_->setText(t.isEmpty() ? QString() : "What tripped it: " + t.mid(0, 6).join("; ") + (t.size() > 6 ? QString("; and %1 more").arg(t.size() - 6) : ""));

    QVector<double> cpu, mem, temp, guardLine;
    double s0 = -1, s1 = -1;
    for (int i = 0; i < int(e.samples.size()); ++i) {
        const auto& p = e.samples[i];
        cpu << p.cpu;
        mem << p.mem_pct;
        temp << (p.temp >= 0 ? p.temp / 110.0 * 100 : std::nan(""));
        guardLine << (p.guard >= 0 ? p.guard / 110.0 * 100 : std::nan(""));
        if (s0 < 0 && p.t >= e.start) s0 = i;
        if (p.t <= e.end) s1 = i;
    }
    graph_->setCapacity(std::max(2, int(cpu.size())));
    graph_->setRange(0, 100);
    graph_->setFrameColor(c);
    graph_->setSpans(s0 >= 0 && s1 > s0 ? QVector<QPair<double, double>>{{s0, s1}} : QVector<QPair<double, double>>{});
    graph_->setCornerText("", "");
    graphNote_->setText(QString("<span style='color:%1'>━ CPU</span>&nbsp; <span style='color:%2'>━ Memory</span>&nbsp; "
                                "<span style='color:%3'>━ Temperature</span>&nbsp; <span style='color:%4'>━ Shutdown sensor (top = 110 °C)</span>"
                                "&nbsp;&nbsp; Shaded: the event. Before it: the lead-up.")
                            .arg(P.cpu.name(), P.memory.name(), P.temp.name(), P.alert.name()));
    graph_->setSeries({{mem, P.memory, 1.6, false}, {temp, P.temp, 1.4, false}, {guardLine, P.alert, 1.4, false}, {cpu, P.cpu, 2.0, false}});
    culprits_->setCulprits(e.culprits, e.primary == 0 ? as::KindCpu : e.primary, e.open);
}

void HistoryPage::showEvent(int64_t id) {
    showingOpen_ = false;
    current_ = id;
    as::Event e = store_->event(id);
    if (!e.id) return;
    timeline_->setSelected(e.start, e.end);
    fill(e);
}

void HistoryPage::showOpen() {
    showingOpen_ = true;
    current_ = -1;
    if (auto e = Live::get().openEvent) fill(*e);
    else {
        detail_->setTitle("Event in progress");
        summary_->setText("Recording now. The details fill in within a couple of seconds.");
    }
}

void HistoryPage::showMoment(double t) {
    showingOpen_ = false;
    current_ = 0;
    timeline_->setSelected(t, t + 1);
    auto top = store_->minute_top(t - 60, t + 60);
    auto samples = store_->samples(t - 150, t + 150);
    detail_->setTitle("At " + QDateTime::fromSecsSinceEpoch(qint64(t)).toString("h:mm AP"));
    meta_->setText(QDateTime::fromSecsSinceEpoch(qint64(t)).toString("dddd MMM d") + ". No event was recorded at this moment.");
    double cpu = 0, mem = 0;
    int n = 0;
    QVector<double> c, m;
    for (const auto& s : samples) {
        c << s.cpu;
        m << s.mem_pct;
        if (std::abs(s.t - t) <= 60) cpu += s.cpu, mem += s.mem_pct, ++n;
    }
    summary_->setText(n ? QString("Around then the CPU averaged %1 and memory use was %2. These programs used the most:")
                              .arg(pct(cpu / n, 0), pct(mem / n, 0))
                        : QString("Nothing was recorded at that moment; ActivityScope was not running or the history has expired."));
    triggers_->clear();
    graph_->setSpans({});
    graph_->setCornerText("", "");
    graphNote_->setText("Five minutes around the moment you picked");
    graph_->setCapacity(std::max(2, int(c.size())));
    graph_->setSeries({{m, pal().memory, 1.6, false}, {c, pal().cpu, 2.0, false}});
    double total = 0;
    for (const auto& x : top) total += x.cpu_seconds;
    for (auto& x : top) x.share = total > 0 ? x.cpu_seconds / total : 0;
    if (top.size() > 10) top.resize(10);
    culprits_->setCulprits(top, as::KindCpu, false);
}

#include "historypage.moc"
