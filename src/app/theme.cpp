#include "theme.h"

#include <QFontDatabase>

QColor withAlpha(QColor c, int alpha) {
    c.setAlpha(alpha);
    return c;
}

namespace {

Palette dark() {
    Palette p;
    p.name = "Dark";
    p.window = QColor("#16181d");
    p.panel = QColor("#1e2127");
    p.panelHi = QColor("#262a31");
    p.border = QColor("#30353d");
    p.sidebar = QColor("#1a1c21");
    p.selection = QColor("#283a63");
    p.text = QColor("#e8eaee");
    p.dim = QColor("#9097a3");
    p.faint = QColor("#5b616c");
    p.grid = QColor(255, 255, 255, 18);
    p.cpu = QColor("#2bf24f");
    p.kernel = QColor("#ff2e2e");
    p.clock = QColor("#ff2e2e");
    p.temp = QColor("#ffc400");
    p.memory = QColor("#b24dff");
    p.gpu = QColor("#3d7dff");
    p.disk = QColor("#00e6a0");
    p.network = QColor("#3a72ff");
    p.energy = QColor("#ffdd00");
    p.thermal = QColor("#ff7300");
    p.fan = QColor("#00d5ff");
    p.alert = QColor("#ff1f4b");
    p.good = QColor("#1fe35a");
    return p;
}

Palette light() {
    Palette p = dark();
    p.name = "Light";
    p.window = QColor("#eef0f3");
    p.panel = QColor("#ffffff");
    p.panelHi = QColor("#f5f6f8");
    p.border = QColor("#d6dae0");
    p.sidebar = QColor("#e6e9ee");
    p.selection = QColor("#cfe0ff");
    p.text = QColor("#14171c");
    p.dim = QColor("#5a616d");
    p.faint = QColor("#9aa1ac");
    p.grid = QColor(0, 0, 0, 22);
    p.cpu = QColor("#16a34a");
    p.memory = QColor("#9333ea");
    p.gpu = QColor("#2563eb");
    p.disk = QColor("#059669");
    p.network = QColor("#1d4ed8");
    p.energy = QColor("#ca8a04");
    p.temp = QColor("#d97706");
    p.thermal = QColor("#ea580c");
    p.fan = QColor("#0891b2");
    p.kernel = QColor("#f05252");
    p.clock = QColor("#f05252");
    p.alert = QColor("#f43f5e");
    p.good = QColor("#22c55e");
    return p;
}

Palette phosphor(const QString& name, QColor hue, QColor bg) {
    Palette p = dark();
    p.name = name;
    p.phosphor = true;
    p.window = bg;
    p.panel = bg.lighter(135);
    p.panelHi = bg.lighter(175);
    p.border = withAlpha(hue, 70);
    p.sidebar = bg.lighter(115);
    p.selection = withAlpha(hue, 55);
    p.text = hue.lighter(130);
    p.dim = withAlpha(hue, 170);
    p.faint = withAlpha(hue, 95);
    p.grid = withAlpha(hue, 22);
    for (QColor* c : {&p.cpu, &p.clock, &p.temp, &p.memory, &p.gpu, &p.disk, &p.network, &p.energy, &p.thermal, &p.fan, &p.good})
        *c = hue;
    p.kernel = hue.darker(160);
    p.alert = hue.lighter(150);
    return p;
}

Palette byName(const QString& n) {
    if (n == "Light") return light();
    if (n == "Green") return phosphor("Green", QColor("#4dff7a"), QColor("#06120a"));
    if (n == "Amber") return phosphor("Amber", QColor("#ffb52e"), QColor("#140c02"));
    if (n == "Blue") return phosphor("Blue", QColor("#5cc8ff"), QColor("#030d17"));
    if (n == "Mono") {
        Palette p = phosphor("Mono", QColor("#d9dde3"), QColor("#121316"));
        p.text = QColor("#f2f3f5");
        return p;
    }
    return dark();
}

}  // namespace

Theme& Theme::get() {
    static Theme t;
    return t;
}

Theme::Theme() : p_(dark()) {
    int id = QFontDatabase::addApplicationFont(":/assets/fonts/Oxanium.ttf");
    auto fams = QFontDatabase::applicationFontFamilies(id);
    heading_family_ = fams.isEmpty() ? QString() : fams.first();
}

QStringList Theme::names() { return {"Dark", "Light", "Green", "Amber", "Blue", "Mono"}; }

void Theme::set(const QString& name) {
    p_ = byName(name);
    emit changed();
}

QFont Theme::heading(int px) const {
    QFont f(heading_family_.isEmpty() ? QStringLiteral("Sans Serif") : heading_family_);
    f.setPixelSize(px);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::PercentageSpacing, 104);
    return f;
}

QFont Theme::body(int px, int weight) const {
    QFont f;
    f.setPixelSize(px);
    f.setWeight(QFont::Weight(weight));
    // Tabular digits keep live numbers from shifting as they change.
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

// Widgets that paint from Qt's palette (scroll areas, menus, spin boxes)
// must match the theme too, or the desktop's own colours show through.
QPalette Theme::palette() const {
    const Palette& p = p_;
    QPalette q;
    q.setColor(QPalette::Window, p.window);
    q.setColor(QPalette::Base, p.panel);
    q.setColor(QPalette::AlternateBase, p.panelHi);
    q.setColor(QPalette::Button, p.panelHi);
    q.setColor(QPalette::WindowText, p.text);
    q.setColor(QPalette::Text, p.text);
    q.setColor(QPalette::ButtonText, p.text);
    q.setColor(QPalette::BrightText, p.alert);
    q.setColor(QPalette::PlaceholderText, p.faint);
    q.setColor(QPalette::Highlight, p.selection);
    q.setColor(QPalette::HighlightedText, p.text);
    q.setColor(QPalette::ToolTipBase, p.panelHi);
    q.setColor(QPalette::ToolTipText, p.text);
    q.setColor(QPalette::Link, p.gpu);
    q.setColor(QPalette::Mid, p.border);
    q.setColor(QPalette::Dark, p.window.darker(120));
    q.setColor(QPalette::Light, p.panelHi.lighter(110));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        q.setColor(QPalette::Disabled, role, p.faint);
    return q;
}

QString Theme::styleSheet() const {
    const Palette& p = p_;
    auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
    return QString(R"(
QWidget { color: %1; background: transparent; }
QMainWindow, #root { background: %2; }
QToolTip { color: %1; background: %3; border: 1px solid %4; padding: 4px; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: %3; border: 1px solid %4; border-radius: 6px; padding: 5px 9px; selection-background-color: %5; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %6; }
QComboBox QAbstractItemView { background: %3; border: 1px solid %4; selection-background-color: %5; }
QPushButton { background: %3; border: 1px solid %4; border-radius: 6px; padding: 6px 14px; }
QPushButton:hover { background: %7; }
QPushButton:pressed { background: %5; }
QPushButton:focus { border-color: %6; }
QCheckBox:focus { color: %6; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
QPushButton#danger { color: %8; border-color: %9; }
QPushButton#danger:hover { background: %10; }
QPushButton:disabled, QPushButton#danger:disabled { color: %11; border-color: %4; background: transparent; }
QScrollBar:vertical { width: 10px; background: %14; margin: 0; border: none; }
QScrollBar::handle:vertical { background: %4; border-radius: 4px; min-height: 30px; }
QScrollBar:horizontal { height: 10px; background: %14; margin: 0; border: none; }
QScrollBar::handle:horizontal { background: %4; border-radius: 4px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: %14; }
QHeaderView::section { background: %2; color: %12; border: none; border-bottom: 1px solid %4;
    padding: 7px 8px; font-size: 13px; font-weight: 600; }
QTreeView, QListView { background: transparent; border: none; alternate-background-color: %13; outline: 0; }
QTreeView::item, QListView::item { padding: 4px 2px; }
QTreeView::item:selected, QListView::item:selected { background: %5; color: %1; }
QMenu { background: %3; border: 1px solid %4; padding: 4px; }
QMenu::item { padding: 6px 18px; border-radius: 4px; }
QMenu::item:selected { background: %5; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %4; border-radius: 4px; background: %3; }
QCheckBox::indicator:checked { background: %6; border-color: %6; }
QSlider::groove:horizontal { height: 4px; background: %4; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0; border-radius: 7px; background: %6; }
)")
        .arg(c(p.text), c(p.window), c(p.panelHi), c(p.border), c(p.selection), c(p.gpu), c(p.selection.lighter(115)))
        .arg(c(p.alert), c(withAlpha(p.alert, 120)), c(withAlpha(p.alert, 40)), c(p.faint), c(p.text))
        .arg(c(withAlpha(p.panelHi, 90)), c(p.panel));
}
