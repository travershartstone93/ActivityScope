#include "popout.h"

#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QUrl>
#include <QWindow>

#include "../format.h"
#include "../live.h"
#include "../theme.h"

QString Popout::skinsFolder() { return QDir::homePath() + "/.config/activityscope/skins"; }

QList<Skin> Popout::skins() {
    QList<Skin> out;
    Skin theme;
    theme.id = "theme";
    theme.name = "Match the app";
    theme.followsTheme = true;
    out << theme;
    auto make = [](const char* id, const char* name, const char* win, const char* text, const char* accent) {
        Skin s;
        s.id = id;
        s.name = name;
        s.window = QColor(win);
        s.panel = s.window.lighter(150);
        s.text = QColor(text);
        s.dim = QColor(accent).darker(140);
        s.cpu = s.memory = s.gpu = s.heat = s.fan = QColor(accent);
        return s;
    };
    out << make("console", "Green console", "#06120a", "#c8ffd6", "#4dff7a") << make("amber", "Amber console", "#140c02", "#ffe7b8", "#ffb52e");
    Skin glass;
    glass.id = "glass";
    glass.name = "Dark glass";
    glass.window = QColor(14, 16, 20, 200);
    glass.panel = QColor(255, 255, 255, 18);
    glass.text = QColor("#f2f4f8");
    glass.dim = QColor("#9aa3b2");
    glass.cpu = QColor("#4ade80");
    glass.memory = QColor("#c084fc");
    glass.gpu = QColor("#60a5fa");
    glass.heat = QColor("#fb7185");
    glass.fan = QColor("#22d3ee");
    out << glass;
    // User skins: any colour left out is taken from the app theme.
    for (const auto& f : QDir(skinsFolder()).entryList({"*.json"}, QDir::Files)) {
        QFile in(skinsFolder() + "/" + f);
        if (!in.open(QIODevice::ReadOnly) || in.size() > 64 * 1024) continue;
        auto o = QJsonDocument::fromJson(in.readAll()).object();
        if (o.isEmpty()) continue;
        Skin s;
        s.id = "file:" + f;
        s.name = o.value("name").toString(f.chopped(5));
        s.followsTheme = true;
        auto col = [&](const char* key, QColor& into) {
            QColor c(o.value(key).toString());
            if (c.isValid()) into = c;
        };
        col("window", s.window);
        col("panel", s.panel);
        col("text", s.text);
        col("dim", s.dim);
        col("cpu", s.cpu);
        col("memory", s.memory);
        col("gpu", s.gpu);
        col("heat", s.heat);
        col("fan", s.fan);
        out << s;
    }
    return out;
}

Popout::Popout(QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle("ActivityScope popout");
    setFixedSize(300, 210);
    QSettings st;
    skinId_ = st.value("popout/skin", "theme").toString();
    if (st.contains("popout/pos")) move(st.value("popout/pos").toPoint());
}

Skin Popout::skin() const {
    Skin s;
    for (const auto& k : skins())
        if (k.id == skinId_) s = k;
    if (s.id.isEmpty()) s = skins().first();
    if (s.followsTheme) {
        const Palette& P = pal();
        auto fill = [](QColor& c, QColor from) {
            if (!c.isValid()) c = from;
        };
        fill(s.window, withAlpha(P.window, 235));
        fill(s.panel, P.panelHi);
        fill(s.text, P.text);
        fill(s.dim, P.dim);
        fill(s.cpu, P.cpu);
        fill(s.memory, P.memory);
        fill(s.gpu, P.gpu);
        fill(s.heat, P.thermal);
        fill(s.fan, P.fan);
    }
    return s;
}

void Popout::refresh() {
    if (isVisible()) update();
}

void Popout::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    Skin k = skin();
    p.setPen(QPen(withAlpha(k.dim, 120), 1));
    p.setBrush(k.window);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    const auto& s = Live::get().now();
    p.setFont(Theme::get().heading(13));
    p.setPen(k.dim);
    p.drawText(QRect(14, 8, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter, "ActivityScope");
    if (!s) return;
    const auto& g = s->guard;
    double gpu = -1;
    for (const auto& x : s->gpus)
        if (x.util.ok()) gpu = std::max(gpu, x.util.get());
    double fan = -1;
    for (const auto& f : s->power.fans)
        if (f.percent.ok()) fan = std::max(fan, double(f.percent.get()));
    struct Row { QString label, value; double frac; QColor color; };
    QList<Row> rows = {{"CPU", pct(s->cpu.total, 0), s->cpu.total / 100, k.cpu},
                       {"Memory", pct(s->mem.percent(), 0), s->mem.percent() / 100, k.memory},
                       {"GPU", gpu >= 0 ? pct(gpu, 0) : "idle", gpu >= 0 ? gpu / 100 : 0, k.gpu},
                       {"Heat", g.temp.ok() ? QString("%1 / %2 °C").arg(g.temp.get(), 0, 'f', 0).arg(g.limit.get(), 0, 'f', 0) : "n/a",
                        g.temp.ok() ? g.temp.get() / g.limit.get() : 0, g.margin.ok() && g.margin.get() <= 10 ? QColor("#f43f5e") : k.heat},
                       {"Fan", fan >= 0 ? pct(fan, 0) : "n/a", fan >= 0 ? fan / 100 : 0, k.fan}};
    int y = 36;
    for (const auto& r : rows) {
        p.setFont(Theme::get().body(13));
        p.setPen(k.text);
        p.drawText(QRect(14, y, 70, 26), Qt::AlignLeft | Qt::AlignVCenter, r.label);
        p.setFont(Theme::get().body(13, QFont::Bold));
        p.setPen(r.color.lighter(115));
        p.drawText(QRect(width() - 104, y, 90, 26), Qt::AlignRight | Qt::AlignVCenter, r.value);
        // Segment bar between the label and the value.
        const int x0 = 80, x1 = width() - 110, seg = 8, gap = 3;
        int n = (x1 - x0 + gap) / (seg + gap), lit = int(std::round(std::clamp(r.frac, 0.0, 1.0) * n));
        for (int i = 0; i < n; ++i) {
            QRectF b(x0 + i * (seg + gap), y + 8, seg, 11);
            p.setPen(Qt::NoPen);
            p.setBrush(i < lit ? r.color : withAlpha(r.color, 40));
            p.drawRoundedRect(b, 2, 2);
        }
        y += 33;
    }
}

void Popout::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && windowHandle()) windowHandle()->startSystemMove();
}

void Popout::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    auto* skinMenu = menu.addMenu("Skin");
    for (const auto& s : skins()) {
        auto* a = skinMenu->addAction(s.name, this, [this, id = s.id] {
            skinId_ = id;
            QSettings().setValue("popout/skin", id);
            update();
        });
        a->setCheckable(true);
        a->setChecked(s.id == skinId_);
    }
    skinMenu->addSeparator();
    skinMenu->addAction("Open skins folder", this, [] {
        QDir().mkpath(skinsFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(skinsFolder()));
    });
    menu.addAction("Close", this, &QWidget::close);
    menu.exec(e->globalPos());
}

void Popout::moveEvent(QMoveEvent* e) {
    QWidget::moveEvent(e);
    QSettings().setValue("popout/pos", pos());
}

void Popout::closeEvent(QCloseEvent* e) {
    QSettings().setValue("popout/open", false);
    QWidget::closeEvent(e);
}
