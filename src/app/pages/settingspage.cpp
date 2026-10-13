#include "settingspage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "../format.h"
#include "../recorder.h"
#include "../theme.h"
#include "../widgets/card.h"
#include "conkyfeed.h"
#include "store.h"

namespace {

// Every threshold, its setting key, a plain label, its unit and range.
struct Knob {
    const char* key;
    const char* label;
    const char* unit;
    double as::Triggers::*field;
    double lo, hi, step;
};

const Knob kKnobs[] = {
    {"cpu_total", "Whole CPU busier than", "%", &as::Triggers::cpu_total, 10, 100, 5},
    {"core", "One core pinned at", "%", &as::Triggers::core, 50, 100, 1},
    {"proc_cpu", "One program using more than", "% of the machine", &as::Triggers::proc_cpu, 5, 100, 5},
    {"pressure", "Programs waiting on memory more than", "% of the time", &as::Triggers::pressure, 0.5, 50, 0.5},
    {"swapin_kbps", "Swapping back in faster than", "KB/s", &as::Triggers::swapin_kbps, 64, 100000, 64},
    {"avail_drop_mb", "Free memory falling by", "MB in 10 s", &as::Triggers::avail_drop_mb, 100, 16000, 100},
    {"proc_growth_mb", "One program growing by", "MB in 10 s", &as::Triggers::proc_growth_mb, 50, 16000, 50},
    {"disk_busy", "A disk busy more than", "% of the time", &as::Triggers::disk_busy, 10, 100, 5},
    {"disk_mbps", "A disk moving more than", "MB/s", &as::Triggers::disk_mbps, 5, 5000, 5},
    {"gpu", "GPU load above", "%", &as::Triggers::gpu, 10, 100, 5},
    {"vram_jump_mb", "Video memory jumping by", "MB", &as::Triggers::vram_jump_mb, 100, 32000, 100},
    {"temp", "CPU temperature at or above", "°C", &as::Triggers::temp, 50, 110, 1},
    {"temp_rise", "CPU temperature rising by", "°C in 30 s", &as::Triggers::temp_rise, 3, 60, 1},
    {"fan", "Fan faster than", "%", &as::Triggers::fan, 10, 100, 5},
    {"fan_step", "Fan stepping up by", "points in 30 s", &as::Triggers::fan_step, 5, 100, 5},
};

QString unitPath() {
    return QDir::homePath() + "/.config/systemd/user/activityscope.service";
}

}  // namespace

as::Triggers SettingsPage::savedTriggers() {
    as::Triggers t;
    QSettings st;
    st.beginGroup("triggers");
    for (const auto& k : kKnobs) t.*(k.field) = st.value(k.key, t.*(k.field)).toDouble();
    st.endGroup();
    return t;
}

void SettingsPage::applySaved(Recorder* r) {
    QSettings st;
    as::Triggers t = savedTriggers();
    int ms = st.value("interval_ms", 1000).toInt();
    double hours = st.value("detail_hours", 24).toDouble();
    double days = st.value("event_days", 30).toDouble();
    double logDays = st.value("log_days", 90).toDouble();
    QStringList allowed = st.value("allowed_sharers").toStringList();
    QMetaObject::invokeMethod(r, [r, t, ms, hours, days, logDays, allowed] {
        r->setAllowedSharers(allowed);
        r->setTriggers(t);
        r->setInterval(ms);
        r->setRetention(hours, days, logDays);
    }, Qt::QueuedConnection);
}

SettingsPage::SettingsPage(Recorder* r, QWidget* parent) : QWidget(parent), recorder_(r) {
    const Palette& P = pal();
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 0, 22, 14);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* grid = new QGridLayout(inner);
    grid->setContentsMargins(0, 0, 8, 0);
    grid->setSpacing(14);
    scroll->setWidget(inner);
    outer->addWidget(scroll);
    QSettings st;
    auto note = [&](const QString& t) {
        auto* l = new QLabel(t);
        l->setWordWrap(true);
        l->setStyleSheet(QString("color:%1; font-size:13px;").arg(P.dim.name()));
        return l;
    };

    auto* look = new Card("palette", "Appearance");
    auto* lf = new QFormLayout;
    auto* theme = new QComboBox;
    theme->addItems(Theme::names());
    theme->setCurrentText(P.name);
    connect(theme, &QComboBox::currentTextChanged, this, [](const QString& t) {
        QSettings().setValue("theme", t);
        QMetaObject::invokeMethod(&Theme::get(), [t] { Theme::get().set(t); }, Qt::QueuedConnection);
    });
    lf->addRow("Theme", theme);
    look->body()->addLayout(lf);
    look->body()->addWidget(note("Dark and Light use a colour per resource. Green, Amber, Blue and Mono turn the whole console one phosphor colour."));
    auto* left = new QVBoxLayout;
    left->setSpacing(14);
    left->addWidget(look);

    auto* rec = new Card("activity", "Recording");
    auto* rf = new QFormLayout;
    auto* interval = new QComboBox;
    interval->addItem("Every half second", 500);
    interval->addItem("Every second", 1000);
    interval->addItem("Every 2 seconds", 2000);
    interval->setCurrentIndex(std::max(0, interval->findData(st.value("interval_ms", 1000))));
    connect(interval, &QComboBox::currentIndexChanged, this, [this, interval] {
        QSettings().setValue("interval_ms", interval->currentData());
        applySaved(recorder_);
    });
    auto* hours = new QSpinBox;
    hours->setRange(1, 168);
    hours->setSuffix(" hours");
    hours->setValue(st.value("detail_hours", 24).toInt());
    auto* days = new QSpinBox;
    days->setRange(1, 365);
    days->setSuffix(" days");
    days->setValue(st.value("event_days", 30).toInt());
    connect(hours, &QSpinBox::valueChanged, this, [this](int v) {
        QSettings().setValue("detail_hours", v);
        applySaved(recorder_);
    });
    connect(days, &QSpinBox::valueChanged, this, [this](int v) {
        QSettings().setValue("event_days", v);
        applySaved(recorder_);
    });
    auto* login = new QCheckBox("Start recording when I log in");
    login->setChecked(QFile::exists(unitPath()) &&
                      QFile::exists(QDir::homePath() + "/.config/systemd/user/graphical-session.target.wants/activityscope.service"));
    loginMsg_ = note("");
    connect(login, &QCheckBox::toggled, this, &SettingsPage::setLoginStart);
    rf->addRow("Measure", interval);
    rf->addRow("Keep full detail for", hours);
    rf->addRow("Keep events for", days);
    auto* logDays = new QSpinBox;
    logDays->setRange(7, 730);
    logDays->setSuffix(" days");
    logDays->setValue(st.value("log_days", 90).toInt());
    connect(logDays, &QSpinBox::valueChanged, this, [this](int v) {
        QSettings().setValue("log_days", v);
        applySaved(recorder_);
    });
    rf->addRow("Keep the log for", logDays);
    rec->body()->addLayout(rf);
    rec->body()->addWidget(login);
    rec->body()->addWidget(loginMsg_);
    rec->body()->addWidget(note("Closing the window keeps recording in the tray. Quit from the tray icon's menu stops it."));
    left->addWidget(rec);

    auto* heat = new Card("thermometer", "Shutdown warnings");
    auto* hf = new QFormLayout;
    auto* warnOn = new QCheckBox("Warn me before the system shuts itself down for heat");
    warnOn->setChecked(st.value("warn_enabled", true).toBool());
    connect(warnOn, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("warn_enabled", on); });
    auto* warnMargin = new QDoubleSpinBox;
    warnMargin->setRange(1, 30);
    warnMargin->setDecimals(0);
    warnMargin->setSuffix(" °C below the shutdown point");
    warnMargin->setValue(st.value("warn_margin", 5).toDouble());
    connect(warnMargin, &QDoubleSpinBox::valueChanged, this, [](double v) { QSettings().setValue("warn_margin", v); });
    auto* warnSecs = new QSpinBox;
    warnSecs->setRange(5, 120);
    warnSecs->setSuffix(" seconds away at the current climb");
    warnSecs->setValue(st.value("warn_seconds", 20).toInt());
    connect(warnSecs, &QSpinBox::valueChanged, this, [](int v) { QSettings().setValue("warn_seconds", v); });
    hf->addRow("Warn at", warnMargin);
    hf->addRow("or when", warnSecs);
    heat->body()->addWidget(warnOn);
    heat->body()->addLayout(hf);
    heat->body()->addWidget(note("The shutdown point is read from the system: the kernel turns the machine off when its critical "
                                 "thermal zone reaches its limit. The warning names the process using the most CPU and offers to "
                                 "pause it; a paused process keeps its work and carries on when you resume it."));
    left->addWidget(heat);

    auto* share = new Card("globe", "Hidden activity");
    allowedLbl_ = note("");
    resetAllowed_ = new QPushButton("Stop allowing them");
    connect(resetAllowed_, &QPushButton::clicked, this, [this] {
        QSettings().remove("allowed_sharers");
        applySaved(recorder_);
        refreshAllowed();
    });
    refreshAllowed();
    auto* allowedLbl = allowedLbl_;
    auto* resetAllowed = resetAllowed_;
    share->body()->addWidget(note("You are warned when a program talks to 8 or more different computers within 10 minutes, the "
                                  "way torrent clients do. Allowing a program from the warning silences it."));
    share->body()->addWidget(allowedLbl);
    auto* keepTray = new QCheckBox("Never let applications hide their tray icons");
    keepTray->setChecked(st.value("keep_tray_visible", true).toBool());
    keepTray->setToolTip("When an application adds an icon to the system tray, it is set to always show, and the Log notes it.");
    connect(keepTray, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("keep_tray_visible", on); });
    share->body()->addWidget(keepTray);
    share->body()->addWidget(resetAllowed, 0, Qt::AlignLeft);
    left->addWidget(share);

    auto* data = new Card("server", "Where the history lives");
    QString db = QString::fromStdString(as::default_db_path());
    dbInfo_ = note(QString("History database: %1 (%2)\nConky feed: %3/conky.txt, rewritten every sample, kept in memory.")
                       .arg(db, bytes(double(QFileInfo(db).size() + QFileInfo(db + "-wal").size())), qs(as::runtime_dir())));
    dbInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* open = new QPushButton("Open folder");
    connect(open, &QPushButton::clicked, this, [db] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(db).absolutePath())); });
    data->body()->addWidget(dbInfo_);
    data->body()->addWidget(open, 0, Qt::AlignLeft);
    data->body()->addWidget(note("Anyone, including Claude, can answer \"what just happened?\" by reading this database, "
                                 "for example: sqlite3 " + db + " \"select summary from events order by start desc limit 5\""));
    left->addWidget(data);
    left->addStretch();
    grid->addLayout(left, 0, 0);

    auto* trig = new Card("zap", "What counts as an event");
    trig->body()->addWidget(note("An event starts when any of these holds for a few seconds, and ends after 30 calm seconds. "
                                 "Lower numbers catch more."));
    auto* tf = new QFormLayout;
    tf->setVerticalSpacing(8);
    as::Triggers t = savedTriggers();
    std::vector<QDoubleSpinBox*> boxes;
    for (const auto& k : kKnobs) {
        auto* box = new QDoubleSpinBox;
        boxes.push_back(box);
        box->setRange(k.lo, k.hi);
        box->setSingleStep(k.step);
        box->setDecimals(k.step < 1 ? 1 : 0);
        box->setSuffix(QString(" ") + k.unit);
        box->setValue(t.*(k.field));
        box->setMinimumWidth(210);
        box->setFixedHeight(32);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, key = k.key](double v) {
            QSettings s;
            s.beginGroup("triggers");
            s.setValue(key, v);
            s.endGroup();
            applySaved(recorder_);
        });
        tf->addRow(k.label, box);
    }
    // Three starting points; every number stays editable below.
    const std::map<QString, double> normal = {{"cpu_total", 70}, {"core", 98}, {"proc_cpu", 40}, {"pressure", 5},
                                              {"swapin_kbps", 4096}, {"avail_drop_mb", 1000}, {"proc_growth_mb", 600},
                                              {"disk_busy", 80}, {"disk_mbps", 100}, {"gpu", 70}, {"vram_jump_mb", 1000},
                                              {"temp", 88}, {"temp_rise", 30}, {"fan", 90}, {"fan_step", 30}};
    const std::map<QString, double> quiet = {{"cpu_total", 85}, {"core", 100}, {"proc_cpu", 60}, {"pressure", 10},
                                             {"swapin_kbps", 8192}, {"avail_drop_mb", 2000}, {"proc_growth_mb", 1000},
                                             {"disk_busy", 90}, {"disk_mbps", 200}, {"gpu", 85}, {"vram_jump_mb", 2000},
                                             {"temp", 92}, {"temp_rise", 35}, {"fan", 95}, {"fan_step", 40}};
    auto applyPreset = [boxes](const std::map<QString, double>* preset) {
        as::Triggers defaults;
        for (std::size_t i = 0; i < std::size(kKnobs); ++i) {
            double v = defaults.*(kKnobs[i].field);
            if (preset) {
                auto it = preset->find(kKnobs[i].key);
                if (it != preset->end()) v = it->second;
            }
            boxes[i]->setValue(v);  // saving and applying happen through the box
        }
    };
    auto* presets = new QHBoxLayout;
    presets->setSpacing(8);
    for (auto [label, preset] : std::initializer_list<std::pair<const char*, const std::map<QString, double>*>>{
             {"Sensitive (default)", nullptr}, {"Normal", &normal}, {"Quiet", &quiet}}) {
        auto* b = new QPushButton(label);
        b->setToolTip(preset ? "Raise the thresholds so only bigger things count as events." : "The defaults: catch as much as possible.");
        connect(b, &QPushButton::clicked, this, [applyPreset, preset] { applyPreset(preset); });
        presets->addWidget(b);
    }
    presets->addStretch(1);
    trig->body()->addLayout(presets);
    trig->body()->addLayout(tf);
    grid->addWidget(trig, 0, 1, Qt::AlignTop);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
}

void SettingsPage::setLoginStart(bool on) {
    QString path = unitPath();
    if (on) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QString("[Unit]\n"
                            "Description=ActivityScope: records what caused CPU, memory, disk, GPU and heat spikes\n"
                            "PartOf=graphical-session.target\n"
                            "After=graphical-session.target\n\n"
                            "[Service]\n"
                            "ExecStart=%1 --hidden\n"
                            "Restart=on-failure\n"
                            "RestartSec=5\n"
                            "Nice=10\n"
                            "MemoryHigh=300M\n\n"
                            "[Install]\n"
                            "WantedBy=graphical-session.target\n")
                        .arg(QCoreApplication::applicationFilePath())
                        .toUtf8());
            f.close();
        }
        QProcess::execute("systemctl", {"--user", "daemon-reload"});
        int rc = QProcess::execute("systemctl", {"--user", "enable", "activityscope.service"});
        loginMsg_->setText(rc == 0 ? "ActivityScope will start hidden in the tray at login." : "Could not enable the login service.");
    } else {
        int rc = QProcess::execute("systemctl", {"--user", "disable", "activityscope.service"});
        loginMsg_->setText(rc == 0 ? "ActivityScope will no longer start at login." : "Could not disable the login service.");
    }
}

void SettingsPage::refreshAllowed() {
    // Allow is granted from the notification, so the card re-reads it on show.
    QStringList allowed = QSettings().value("allowed_sharers").toStringList();
    allowedLbl_->setText(allowed.isEmpty() ? "No programs are allowed to share in the background yet."
                                           : "Allowed to share in the background: " + allowed.join(", ") + ".");
    resetAllowed_->setEnabled(!allowed.isEmpty());
}

void SettingsPage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    refreshAllowed();
}
