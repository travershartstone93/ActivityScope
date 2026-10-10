#pragma once
#include <QColor>
#include <QMap>
#include <QWidget>

// Colours for the popout. Built-in skins plus any JSON files in
// ~/.config/activityscope/skins (data only, nothing a skin can run).
struct Skin {
    QString id, name;
    QColor window, panel, text, dim, cpu, memory, gpu, heat, fan;
    bool followsTheme = false;
};

class Popout : public QWidget {
    Q_OBJECT
public:
    explicit Popout(QWidget* parent = nullptr);
    void refresh();
    static QList<Skin> skins();
    static QString skinsFolder();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void closeEvent(QCloseEvent*) override;

private:
    Skin skin() const;
    QString skinId_;
};
