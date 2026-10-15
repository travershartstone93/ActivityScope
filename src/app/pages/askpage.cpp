#include "askpage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "common.h"
#include "store.h"

namespace {

QString dataFile(const char* name) {
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString& c : {app + "/../share/activityscope/unllm/" + name,
                             app + "/../vendor/unllm/data/" + name})
        if (QFileInfo::exists(c)) return c;
    return {};
}

}  // namespace

AskPage::AskPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 0, 22, 14);
    v->setSpacing(10);

    auto* bar = new QHBoxLayout;
    input_ = new QLineEdit;
    input_->setPlaceholderText("Ask in plain words: why did the fan scream last night?");
    input_->setFixedHeight(38);
    goBtn_ = new QPushButton("Ask");
    bar->addWidget(input_, 1);
    bar->addWidget(goBtn_);
    v->addLayout(bar);

    auto* examples = new QHBoxLayout;
    examples->setSpacing(8);
    for (const char* ex : {"why did the fan scream last night", "what crashed this week",
                           "what got installed yesterday", "who used the network today"}) {
        auto* b = new QPushButton(ex);
        connect(b, &QPushButton::clicked, this, [this, ex] {
            input_->setText(ex);
            go();
        });
        examples->addWidget(b);
    }
    examples->addStretch(1);
    v->addLayout(examples);

    answer_ = new QLabel;
    answer_->setWordWrap(true);
    answer_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    answer_->hide();
    v->addWidget(answer_);

    auto* rbox = new QWidget;
    receipts_ = new QVBoxLayout(rbox);
    receipts_->setContentsMargins(0, 0, 0, 0);
    receipts_->setSpacing(6);
    v->addWidget(rbox);
    v->addStretch(1);

    v->addWidget(noteLabel("Answers come from the history database through unllm, the deterministic language library: "
                           "table lookups, search ranking and grammar rules, costing microseconds. No model runs, nothing "
                           "leaves this machine, and a question it does not understand is refused in so many words, with "
                           "the records it did find shown underneath every answer it gives."));

    connect(goBtn_, &QPushButton::clicked, this, &AskPage::go);
    connect(input_, &QLineEdit::returnPressed, this, &AskPage::go);

    if (qEnvironmentVariableIsSet("AS_ASK")) input_->setText(qEnvironmentVariable("AS_ASK"));
}

void AskPage::go() {
    const QString q = input_->text().trimmed();
    if (q.isEmpty()) return;
    if (!ask_) {
        ask_ = std::make_unique<as::Ask>();
        if (!ask_->open(as::default_db_path())) {
            answer_->setText("The history database could not be opened.");
            answer_->show();
            ask_.reset();
            return;
        }
        ask_->load_language(dataFile("synonyms.txt").toStdString(), dataFile("vectors.bin").toStdString());
    }
    as::AskResult r = ask_->ask(q.toStdString(), QDateTime::currentSecsSinceEpoch());

    answer_->setText(QString::fromStdString(r.answer));
    answer_->setStyleSheet(QString("color:%1; font-size:16px; font-weight:600; background:%2; border:1px solid %3; "
                                   "border-radius:8px; padding:14px;")
                               .arg(r.refused ? pal().temp.name() : pal().text.name(), pal().panel.name(), pal().border.name()));
    answer_->show();

    while (auto* item = receipts_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    for (const auto& line : r.receipts) {
        auto* l = new QLabel(QString::fromStdString(line));
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setStyleSheet(QString("color:%1; font-size:13px; border-left:3px solid %2; padding-left:10px;")
                             .arg(pal().dim.name(), pal().gpu.name()));
        receipts_->addWidget(l);
    }
}
