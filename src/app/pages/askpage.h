#pragma once
#include <QWidget>
#include <memory>

class QThread;

#include "askcore.h"

class QLineEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

// Plain questions against the machine's own records, answered by unllm:
// deterministic reading, retrieval and grammar, never silence; a small local
// model checks each answer fits, off the UI thread.
class AskPage : public QWidget {
    Q_OBJECT
public:
    explicit AskPage(QWidget* parent = nullptr);
    ~AskPage() override;
    void refresh() {}

public slots:
    void go();        // a new question: nothing earlier is in force
    void followUp();  // a follow-up to the last answer: its subject, moment and time stay in force

private:
    void run(as::Ask::Mode mode);
    void show_result(const as::AskResult& r);
    QLineEdit* input_;
    QPushButton* goBtn_;
    QPushButton* followBtn_;   // shown once there is an answer to follow up
    bool answered_ = false;    // Enter follows up once there is an answer; before that it asks
    QLabel* answer_;
    QVBoxLayout* receipts_;
    std::unique_ptr<as::Ask> ask_;
    QThread* worker_ = nullptr;  // the question being answered, one at a time
};
