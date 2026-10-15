#pragma once
#include <QWidget>
#include <memory>

#include "askcore.h"

class QLineEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

// Plain questions against the machine's own records, answered by unllm:
// deterministic reading, retrieval and grammar; no model, and never silence.
class AskPage : public QWidget {
    Q_OBJECT
public:
    explicit AskPage(QWidget* parent = nullptr);
    void refresh() {}

public slots:
    void go();

private:
    QLineEdit* input_;
    QPushButton* goBtn_;
    QLabel* answer_;
    QVBoxLayout* receipts_;
    std::unique_ptr<as::Ask> ask_;
};
