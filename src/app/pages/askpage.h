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
    void go();

private:
    void show_result(const as::AskResult& r);
    QLineEdit* input_;
    QPushButton* goBtn_;
    QLabel* answer_;
    QVBoxLayout* receipts_;
    std::unique_ptr<as::Ask> ask_;
    QThread* worker_ = nullptr;  // the question being answered, one at a time
};
