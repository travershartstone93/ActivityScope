#include "logpage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSplitter>
#include <QTextStream>
#include <QDir>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <set>

#include "../format.h"
#include "../live.h"
#include "../widgets/card.h"
#include "common.h"

namespace {

struct Category { const char* id; const char* name; const char* icon; };
const Category kCats[] = {
    {"devices", "Devices", "cable"},          {"storage", "Drives", "hard-drive"},       {"flashing", "Flashing", "zap"},
    {"software", "Software", "package"},       {"admin", "Administrator", "cog"},         {"session", "Power and sessions", "power"},
    {"network", "Network", "globe"},           {"services", "Services", "server"},        {"problems", "Problems", "info"},
    {"programs", "New programs", "list-tree"}, {"resources", "Resource events", "gauge"},
};

const Category* category(const std::string& id) {
    for (const auto& c : kCats)
        if (id == c.id) return &c;
    return &kCats[0];
}

QColor categoryColor(const std::string& id) {
    const Palette& P = pal();
    if (id == "problems") return P.alert;
    if (id == "admin") return P.temp;
    if (id == "flashing") return P.energy;
    if (id == "storage") return P.disk;
    if (id == "network") return P.network;
    if (id == "resources") return P.cpu;
    if (id == "software") return P.memory;
    if (id == "devices") return P.fan;
    return P.dim;
}

}  // namespace

LogPage::LogPage(QWidget* parent) : QWidget(parent), store_(std::make_unique<as::Store>(as::default_db_path())) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(10);

    auto* bar = new QHBoxLayout;
    count_ = noteLabel();
    range_ = new QComboBox;
    range_->addItems({"Today", "Last 7 days", "Last 30 days", "Last 90 days"});
    range_->setCurrentIndex(1);
    search_ = filterBox("Search the log");
    auto* exportBtn = new QPushButton("Export");
    exportBtn->setToolTip("Save the entries shown as a text file, for an investigation or to share.");
    bar->addWidget(count_, 1);
    bar->addWidget(range_);
    bar->addWidget(search_);
    bar->addWidget(exportBtn);
    v->addLayout(bar);

    // One toggle per category; all start on.
    auto* chips = new QHBoxLayout;
    chips->setSpacing(6);
    cats_ = new QButtonGroup(this);
    cats_->setExclusive(false);
    for (int i = 0; i < int(std::size(kCats)); ++i) {
        auto* b = new QPushButton(QIcon(iconPixmap(kCats[i].icon, categoryColor(kCats[i].id), 16)), kCats[i].name);
        b->setCheckable(true);
        b->setChecked(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString("QPushButton { padding:4px 10px; border-radius:13px; font-size:12px; }"
                                 "QPushButton:checked { background:%1; border-color:%2; }")
                             .arg(pal().selection.name(QColor::HexArgb), pal().selection.lighter(140).name()));
        cats_->addButton(b, i);
        chips->addWidget(b);
    }
    chips->addStretch();
    v->addLayout(chips);

    auto* split = new QSplitter(Qt::Vertical);
    split->setStyleSheet("QSplitter::handle { background: transparent; }");
    split->setHandleWidth(12);
    tree_ = styledTree({"When", "Kind", "What happened"});
    tree_->setSortingEnabled(false);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    tree_->header()->resizeSection(0, 190);
    tree_->header()->resizeSection(1, 190);
    split->addWidget(tree_);
    detail_ = new Card("scroll-text", "Pick an entry");
    detail_->setTitleSize(17);
    dWhen_ = noteLabel();
    dText_ = new QLabel("The details and the raw evidence for the entry you pick appear here.");
    dText_->setWordWrap(true);
    dText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dText_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    dText_->setStyleSheet("font-size:14px;");
    detail_->body()->addWidget(dWhen_);
    detail_->body()->addWidget(dText_, 1);
    split->addWidget(detail_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    connect(range_, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty() || t.size() >= 3) reload();
    });
    connect(cats_, &QButtonGroup::idToggled, this, [this] { reload(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { showSelected(); });
    connect(exportBtn, &QPushButton::clicked, this, &LogPage::exportRange);
}

void LogPage::refresh() {
    double now = QDateTime::currentSecsSinceEpoch();
    if (now - loaded_ >= 5) reload();
}

void LogPage::reload() {
    loaded_ = QDateTime::currentSecsSinceEpoch();
    double from;
    if (range_->currentIndex() == 0) from = double(QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch());
    else from = loaded_ - std::array<double, 4>{0, 7, 30, 90}[range_->currentIndex()] * 86400;
    const std::string q = search_->text().toStdString();
    rows_ = store_->log(from, loaded_ + 60, q);
    // Resource events and shutdowns live in their own table; show them here too.
    for (const auto& e : store_->events(from, loaded_ + 60)) {
        std::string text = e.summary;
        auto cut = text.find("). ");
        if (cut != std::string::npos) text = text.substr(cut + 3);
        if (!q.empty() && text.find(q) == std::string::npos) continue;
        std::string first = text.substr(0, text.find(". ") == std::string::npos ? text.size() : text.find(". ") + 1);
        rows_.push_back({e.start, "resources", first, text});
    }
    std::set<std::string> on;
    for (int i = 0; i < int(std::size(kCats)); ++i)
        if (cats_->button(i)->isChecked()) on.insert(kCats[i].id);
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const as::LogEntry& e) { return !on.count(e.category); }), rows_.end());
    std::sort(rows_.begin(), rows_.end(), [](const auto& a, const auto& b) { return a.t > b.t; });

    QString keep = tree_->currentItem() ? tree_->currentItem()->data(0, Qt::UserRole).toString() : QString();
    tree_->clear();
    const QDate today = QDate::currentDate();
    for (int i = 0; i < int(rows_.size()); ++i) {
        const auto& e = rows_[i];
        QDateTime when = QDateTime::fromSecsSinceEpoch(qint64(e.t));
        auto* it = new QTreeWidgetItem(tree_);
        it->setText(0, (when.date() == today ? QString("Today") : when.date() == today.addDays(-1) ? QString("Yesterday") : when.toString("ddd MMM d")) +
                           when.toString(", h:mm:ss AP"));
        const Category* c = category(e.category);
        it->setText(1, c->name);
        it->setIcon(1, QIcon(iconPixmap(c->icon, categoryColor(e.category), 16)));
        it->setText(2, qs(e.title));
        it->setToolTip(2, qs(e.title));
        it->setData(0, Qt::UserRole, QString::number(e.t, 'f', 3) + qs(e.title));
        it->setData(1, Qt::UserRole, i);
        if (e.category == "problems") it->setForeground(2, pal().alert);
        if (it->data(0, Qt::UserRole).toString() == keep) tree_->setCurrentItem(it);
    }
    count_->setText(rows_.empty() ? QString("Nothing logged in this range yet. Plug something in, install a package or run a sudo command, and it appears here.")
                                  : QString("%1 entries").arg(rows_.size()));
}

void LogPage::showSelected() {
    auto* it = tree_->currentItem();
    if (!it) return;
    int i = it->data(1, Qt::UserRole).toInt();
    if (i < 0 || i >= int(rows_.size())) return;
    const auto& e = rows_[i];
    detail_->setTitle(qs(e.title).left(140));
    detail_->setTint(QColor());
    QDateTime when = QDateTime::fromSecsSinceEpoch(qint64(e.t));
    dWhen_->setText(when.toString("dddd MMMM d yyyy, h:mm:ss AP") + ". " + category(e.category)->name + ".");
    dText_->setText(e.detail.empty() ? QString("No further details were recorded.") : qs(e.detail).toHtmlEscaped().replace("\n", "<br>"));
}

void LogPage::exportRange() {
    QString file = QFileDialog::getSaveFileName(this, "Export log", QDir::homePath() + "/activityscope-log-" +
                                                                       QDate::currentDate().toString("yyyy-MM-dd") + ".txt",
                                                "Text files (*.txt)");
    if (file.isEmpty()) return;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        count_->setText("Could not write " + file + ": " + f.errorString());
        return;
    }
    QTextStream out(&f);
    out << "ActivityScope log, exported " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
    for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
        out << QDateTime::fromSecsSinceEpoch(qint64(it->t)).toString("yyyy-MM-dd HH:mm:ss") << "  [" << category(it->category)->name << "]  "
            << qs(it->title) << "\n";
        if (!it->detail.empty())
            for (const auto& line : qs(it->detail).split('\n')) out << "        " << line << "\n";
    }
    count_->setText(QString("Saved %1 entries to %2").arg(rows_.size()).arg(file));
}
