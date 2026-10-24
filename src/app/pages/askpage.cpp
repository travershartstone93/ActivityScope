#include <QDir>
#include "askpage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
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

QString factsFile(const char* name) {
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString& c : {app + "/../share/activityscope/facts/" + name,
                             app + "/../facts/" + name})
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
    for (const char* ex : {"why did the fan scream last night", "how hot is it right now",
                           "what crashed this week", "what is this machine", "what can I ask"}) {
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
                           "table lookups, search ranking and grammar rules. A small local model (qwen3:4b through "
                           "Ollama) then checks that each answer fits the question; it never writes one. Nothing leaves "
                           "this machine, and a question it does not understand is refused in so many words, with the "
                           "records it did find shown underneath every answer it gives."));

    connect(goBtn_, &QPushButton::clicked, this, &AskPage::go);
    connect(input_, &QLineEdit::returnPressed, this, &AskPage::go);

    if (qEnvironmentVariableIsSet("AS_ASK")) input_->setText(qEnvironmentVariable("AS_ASK"));
}

AskPage::~AskPage() {
    if (worker_) worker_->wait();
}

void AskPage::go() {
    const QString q = input_->text().trimmed();
    if (q.isEmpty() || worker_) return;
    if (!ask_) {
        ask_ = std::make_unique<as::Ask>();
        if (!ask_->open(as::default_db_path())) {
            answer_->setText("The history database could not be opened.");
            answer_->show();
            ask_.reset();
            return;
        }
        ask_->load_language(dataFile("synonyms.txt").toStdString(), dataFile("vectors.bin").toStdString(),
                            dataFile("banter.txt").toStdString(), dataFile("rewrites.txt").toStdString());
        ask_->load_conversation(dataFile("followups.txt").toStdString(), dataFile("chatspeak.txt").toStdString());
        // Same folder as the database, same file as the CLI writes.
        std::string db = as::default_db_path();
        std::string dbdir = db.substr(0, db.rfind('/'));
        ask_->set_miss_log(dbdir + "/ask-misses.log");
        ask_->load_facts({dbdir + "/facts/local.tsv", factsFile("base.tsv").toStdString(),
                          dbdir + "/facts/custom.tsv"});
        // The question kinds and their matcher, as the CLI loads them.
        const QString exe = QCoreApplication::applicationDirPath();
        QString kindsDir = exe + "/../share/activityscope/kinds";
        if (!QFileInfo::exists(kindsDir + "/kinds.tsv")) kindsDir = exe + "/../data/kinds";
        QDir().mkpath(QString::fromStdString(dbdir) + "/kinds");
        ask_->load_kinds(kindsDir.toStdString(), dbdir + "/kinds",
                         QDir::homePath().toStdString() + "/.local/share/unllm/models/bge-small-en-v1.5");
        // The final check (askcore.h), as the CLI sets it; ASK_CHECK=off turns it off.
        ask_->set_checker(qEnvironmentVariableIsSet("ASK_CHECK") ? qEnvironmentVariable("ASK_CHECK").toStdString()
                                                                 : std::string("qwen3:4b"));
    }
    // The check can take a few seconds: the question is answered on a worker thread.
    goBtn_->setEnabled(false);
    input_->setEnabled(false);
    const std::string question = q.toStdString();
    const int64_t now = QDateTime::currentSecsSinceEpoch();
    auto result = std::make_shared<as::AskResult>();
    worker_ = QThread::create([this, question, now, result] { *result = ask_->ask(question, now); });
    connect(worker_, &QThread::finished, this, [this, result] {
        worker_->deleteLater();
        worker_ = nullptr;
        goBtn_->setEnabled(true);
        input_->setEnabled(true);
        input_->setFocus();
        show_result(*result);
    });
    worker_->start();
}

void AskPage::show_result(const as::AskResult& r) {
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
