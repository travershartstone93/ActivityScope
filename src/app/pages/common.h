#pragma once
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QStringList>
#include <QTreeWidget>

#include "../icons.h"
#include "../theme.h"

// Small building blocks shared by the list pages, so they all look alike.
inline QTreeWidget* styledTree(const QStringList& headers) {
    auto* t = new QTreeWidget;
    t->setHeaderLabels(headers);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    t->setUniformRowHeights(true);
    t->setSortingEnabled(true);
    t->sortByColumn(0, Qt::AscendingOrder);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setStyleSheet("QTreeView { font-size: 14px; } QTreeView::item { height: 30px; }");
    t->header()->setStretchLastSection(false);
    t->header()->setSectionResizeMode(QHeaderView::Interactive);
    t->setIconSize(QSize(18, 18));
    return t;
}

inline QLineEdit* filterBox(const QString& placeholder) {
    auto* f = new QLineEdit;
    f->setPlaceholderText(placeholder);
    f->addAction(themedIcon("search", pal().dim), QLineEdit::LeadingPosition);
    f->setClearButtonEnabled(true);
    f->setMinimumWidth(320);
    return f;
}

inline QLabel* noteLabel(const QString& text = {}) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color:%1; font-size:13px;").arg(pal().dim.name()));
    return l;
}

// Hides rows that do not contain the filter text in any column.
inline void applyFilter(QTreeWidget* t, const QString& f) {
    for (int i = 0; i < t->topLevelItemCount(); ++i) {
        auto* it = t->topLevelItem(i);
        bool show = f.isEmpty();
        for (int c = 0; c < t->columnCount() && !show; ++c) show = it->text(c).contains(f, Qt::CaseInsensitive);
        it->setHidden(!show);
    }
}

// Runs a short command and returns its output (empty on failure).
inline QString runQuick(const QString& program, const QStringList& args, int ms = 5000) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(ms)) {
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput());
}

// A tree item that sorts numerically on the value stored in UserRole.
class NumItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& o) const override {
        int c = treeWidget() ? treeWidget()->sortColumn() : 0;
        QVariant a = data(c, Qt::UserRole), b = o.data(c, Qt::UserRole);
        if (a.isValid() && b.isValid()) return a.toDouble() < b.toDouble();
        return text(c).compare(o.text(c), Qt::CaseInsensitive) < 0;
    }
};
