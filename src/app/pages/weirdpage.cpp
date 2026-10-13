#include "weirdpage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "common.h"

namespace {

enum { RoleText = Qt::UserRole, RolePid, RoleExtra };

struct Row {
    QString severity, title, where, part, text, extra;
    int pid = 0;
};

QString severityLabel(const QString& s) {
    if (s == "critical") return "Act now";
    if (s == "medium") return "Worth a look";
    return "Minor";
}

QColor severityColor(const QString& s) {
    if (s == "critical") return pal().alert;
    if (s == "medium") return pal().temp;
    return pal().dim;
}

int severityRank(const QString& s) { return s == "critical" ? 0 : s == "medium" ? 1 : 2; }

bool shown(const QString& sev) { return sev == "critical" || sev == "medium" || sev == "low"; }

QString extraFor(const QJsonObject& o) {
    QStringList e;
    if (!o["provenance"].toString().isEmpty()) e << "Provenance: " + o["provenance"].toString();
    if (!o["attack"].toString().isEmpty()) e << "Technique: " + o["attack"].toString() + " (MITRE ATT&CK)";
    if (o["yours"].toBool()) e << "This is one of your own entries.";
    return e.join("\n");
}

// The findings weird's own report shows, section by section. Only rows weird
// judged critical, medium or low appear; info rows stay in the counts.
std::vector<Row> collect(const QJsonObject& root) {
    std::vector<Row> rows;
    auto add = [&](Row r) {
        if (shown(r.severity)) rows.push_back(std::move(r));
    };
    for (const auto& v : root["persist"].toObject()["entries"].toArray()) {
        QJsonObject o = v.toObject();
        QString file = o["file"].toString();
        add({o["severity"].toString(), file.section('/', -1) + " (" + o["source"].toString() + ")", file,
             "Startup and persistence", o["text"].toString(), extraFor(o), 0});
    }
    for (const auto& v : root["processes"].toObject()["processes"].toArray()) {
        QJsonObject p = v.toObject();
        for (const auto& fv : p["findings"].toArray()) {
            QJsonObject f = fv.toObject();
            add({f["severity"].toString(), p["name"].toString() + " (pid " + QString::number(p["pid"].toInt()) + "): " + f["kind"].toString(),
                 p["path"].toString(), "Running programs", f["detail"].toString(), extraFor(p), p["pid"].toInt()});
        }
    }
    for (const auto& v : root["aur"].toObject()["packages"].toArray()) {
        QJsonObject p = v.toObject();
        for (const auto& fv : p["findings"].toArray()) {
            QJsonObject f = fv.toObject();
            add({f["severity"].toString(), p["name"].toString() + ": " + f["kind"].toString(), p["pkgbuild"].toString(),
                 "AUR packages", f["detail"].toString(), extraFor(f), 0});
        }
    }
    for (const auto& v : root["changes"].toObject()["findings"].toArray()) {
        QJsonObject f = v.toObject();
        add({f["severity"].toString(), f["title"].toString().isEmpty() ? f["kind"].toString() : f["title"].toString(),
             f["subject"].toString(), "Changes since the last scan", f["text"].toString().isEmpty() ? f["detail"].toString() : f["text"].toString(),
             extraFor(f), 0});
    }
    for (const char* section : {"hidden", "kernel", "stale"}) {
        for (const auto& v : root[section].toObject()["findings"].toArray()) {
            QJsonObject f = v.toObject();
            add({f["severity"].toString(), f["title"].toString().isEmpty() ? f["kind"].toString() : f["title"].toString(),
                 f["subject"].toString(), QString(section) == "hidden" ? "Hidden processes" : QString(section) == "kernel" ? "Kernel" : "Stale programs",
                 f["text"].toString().isEmpty() ? f["detail"].toString() : f["text"].toString(), extraFor(f), 0});
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return severityRank(a.severity) < severityRank(b.severity); });
    return rows;
}

}  // namespace

WeirdPage::WeirdPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(10);

    auto* bar = new QHBoxLayout;
    status_ = noteLabel("Nothing has been scanned yet. Press Scan now to run weird.");
    scanBtn_ = new QPushButton("Scan now");
    scanBtn_->setToolTip("Runs the weird command as you, never as root. Takes a minute or two.");
    bar->addWidget(status_, 1);
    bar->addWidget(scanBtn_);
    v->addLayout(bar);

    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    progress_->setFixedHeight(8);
    progress_->setRange(0, 0);
    progress_->hide();
    v->addWidget(progress_);

    headline_ = new QLabel;
    headline_->setWordWrap(true);
    headline_->hide();
    v->addWidget(headline_);

    tree_ = styledTree({"Judgement", "Finding", "Where", "Section"});
    tree_->setSortingEnabled(false);  // rows stay in judgement order, worst first
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 150);
    tree_->header()->resizeSection(2, 420);
    tree_->header()->resizeSection(3, 190);
    tree_->setStyleSheet("QTreeView { font-size: 14px; } QTreeView::item { height: 34px; }");
    v->addWidget(tree_, 1);

    detail_ = new QLabel;
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_->hide();
    v->addWidget(detail_);

    auto* actions = new QHBoxLayout;
    coverage_ = noteLabel();
    trustBtn_ = new QPushButton("Trust this program");
    trustBtn_->setToolTip("Runs weird trust on this process: its executable's fingerprint is pinned and scans stay quiet until the file changes.");
    trustBtn_->hide();
    jumpBtn_ = new QPushButton("Show in Processes");
    jumpBtn_->hide();
    actions->addWidget(coverage_, 1);
    actions->addWidget(jumpBtn_);
    actions->addWidget(trustBtn_);
    v->addLayout(actions);

    v->addWidget(noteLabel("weird judges whether what is running and installed can be trusted, from the distribution's own signed package "
                           "data. A copy ships with ActivityScope and runs as you; installing the npm package thatsweird gives a "
                           "self-updating one, which is then preferred. Neither ever gets administrator rights, and nothing is "
                           "scanned unless you press the button."));

    connect(scanBtn_, &QPushButton::clicked, this, &WeirdPage::scan);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) { showDetail(it); });
    connect(jumpBtn_, &QPushButton::clicked, this, [this] {
        if (auto* it = tree_->currentItem()) emit showProcess(it->data(0, RolePid).toInt());
    });
    connect(trustBtn_, &QPushButton::clicked, this, [this] {
        auto* it = tree_->currentItem();
        if (!it) return;
        auto* p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            status_->setText(code == 0 ? "Pinned. Scan again and this program stays quiet until its file changes."
                                       : "weird trust failed: " + QString::fromUtf8(p->readAllStandardError()).trimmed());
            p->deleteLater();
        });
        p->start(weirdPath(), {"trust", it->data(0, RolePid).toString()});
    });

    if (weirdPath().isEmpty()) {
        scanBtn_->setEnabled(false);
        status_->setText("The weird scanner is missing; reinstalling ActivityScope restores the bundled copy.");
    } else if (usingBundled()) {
        status_->setText("Nothing has been scanned yet. Press Scan now to run the bundled weird scanner.");
    }
}

QString WeirdPage::weirdPath() const {
    // A system installation (which updates itself through npm) wins; the
    // copy shipped with ActivityScope is the fallback, so nothing else
    // needs installing.
    QString p = QStandardPaths::findExecutable("weird");
    if (!p.isEmpty()) return p;
    for (const QString& c : {QDir::homePath() + "/.local/bin/weird",
                             QCoreApplication::applicationDirPath() + "/../share/activityscope/thatsweird/bin/weird",
                             QCoreApplication::applicationDirPath() + "/../vendor/thatsweird/bin/weird"})
        if (QFileInfo::exists(c)) return c;
    return {};
}

bool WeirdPage::usingBundled() const {
    return weirdPath().contains("thatsweird");
}

void WeirdPage::scan() {
    if (scanning_) return;
    scanning_ = true;
    scanBtn_->setEnabled(false);
    progress_->show();
    status_->setText("weird is scanning: processes, startup entries, AUR packages, kernel, hidden processes. Takes a minute or two.");
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        scanning_ = false;
        scanBtn_->setEnabled(true);
        progress_->hide();
        // weird's exit code is its verdict: 0 clean, 1 worth a look, 2 act now.
        if (st != QProcess::NormalExit || code > 2) {
            status_->setText("The scan failed: " + QString::fromUtf8(p->readAllStandardError()).trimmed().section('\n', -1));
            p->deleteLater();
            return;
        }
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(p->readAllStandardOutput(), &err);
        if (doc.isNull()) status_->setText("The scan's output could not be read: " + err.errorString());
        else fill(doc);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        scanning_ = false;
        scanBtn_->setEnabled(true);
        progress_->hide();
        status_->setText("The weird scanner could not start; it needs python3.");
        p->deleteLater();
    });
    // -v so the coverage counts cover everything checked, not just findings.
    p->start(weirdPath(), {"--json", "-v"});
}

void WeirdPage::fill(const QJsonDocument& doc) {
    const QJsonObject root = doc.object();
    const auto rows = collect(root);
    tree_->clear();
    detail_->hide();
    trustBtn_->hide();
    jumpBtn_->hide();

    int act = 0, look = 0, minor = 0;
    for (const auto& r : rows) {
        auto* it = new QTreeWidgetItem(tree_);
        it->setText(0, severityLabel(r.severity));
        it->setForeground(0, severityColor(r.severity));
        it->setText(1, r.title);
        it->setText(2, r.where);
        it->setForeground(2, pal().dim);
        it->setText(3, r.part);
        it->setForeground(3, pal().dim);
        it->setIcon(0, iconPixmap("search", severityColor(r.severity), 16));
        it->setData(0, RoleText, r.text);
        it->setData(0, RolePid, r.pid);
        it->setData(0, RoleExtra, r.extra);
        (r.severity == "critical" ? act : r.severity == "medium" ? look : minor)++;
    }
    QString host = root["processes"].toObject()["host"].toString();
    QString head;
    if (rows.empty()) head = "Nothing weird on " + (host.isEmpty() ? "this machine" : host) + ".";
    else {
        QStringList parts;
        if (act) parts << QString("%1 need%2 you to act now").arg(act).arg(act == 1 ? "s" : "");
        if (look) parts << QString("%1 %2 worth a look").arg(look).arg(look == 1 ? "is" : "are");
        if (minor) parts << QString("%1 %2 minor").arg(minor).arg(minor == 1 ? "is" : "are");
        head = parts.join(", ") + ".";
        head[0] = head[0].toUpper();
    }
    headline_->setText(head);
    headline_->setStyleSheet(QString("color:%1; font-size:16px; font-weight:600;").arg(act ? pal().alert.name() : look ? pal().temp.name() : pal().good.name()));
    headline_->show();

    const int procs = root["processes"].toObject()["processes"].toArray().size();
    const int persist = root["persist"].toObject()["entries"].toArray().size();
    const int aur = root["aur"].toObject()["packages"].toArray().size();
    QString base = root["changes"].toObject()["baseline_date"].toString();
    coverage_->setText(QString("Checked %1 running programs, %2 startup entries and %3 AUR packages, plus the kernel and hidden processes.%4")
                           .arg(procs)
                           .arg(persist)
                           .arg(aur)
                           .arg(base.isEmpty() ? QString() : " Changes are measured against your last scan (" + base + ")."));
    status_->setText("Scanned " + QDateTime::currentDateTime().toString("MMM d, HH:mm") + ". These are weird's judgements in its own words.");
}

void WeirdPage::showDetail(QTreeWidgetItem* it) {
    if (!it) {
        detail_->hide();
        trustBtn_->hide();
        jumpBtn_->hide();
        return;
    }
    QString text = it->data(0, RoleText).toString();
    QString extra = it->data(0, RoleExtra).toString();
    detail_->setText(text + (extra.isEmpty() ? "" : "\n\n" + extra));
    detail_->setStyleSheet(QString("color:%1; font-size:14px; background:%2; border:1px solid %3; border-radius:6px; padding:10px;")
                               .arg(pal().text.name(), pal().panel.name(), pal().border.name()));
    detail_->setVisible(!text.isEmpty());
    const int pid = it->data(0, RolePid).toInt();
    trustBtn_->setVisible(pid > 0);
    jumpBtn_->setVisible(pid > 0);
}
