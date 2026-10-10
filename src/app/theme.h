#pragma once
#include <QColor>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

// Every colour the app paints with comes from here, so a theme switch is one call.
struct Palette {
    QString name;
    QColor window, panel, panelHi, border, sidebar, selection;
    QColor text, dim, faint, grid;
    QColor cpu, kernel, clock, temp, memory, gpu, disk, network, energy, thermal, fan, alert, good;
    bool phosphor = false;  // single-hue console look
};

class Theme : public QObject {
    Q_OBJECT
public:
    static Theme& get();
    const Palette& p() const { return p_; }
    static QStringList names();
    void set(const QString& name);
    QFont heading(int px) const;
    QFont body(int px, int weight = QFont::Normal) const;
    QString styleSheet() const;
    QPalette palette() const;

signals:
    void changed();

private:
    Theme();
    Palette p_;
    QString heading_family_;
};

inline const Palette& pal() { return Theme::get().p(); }
QColor withAlpha(QColor c, int alpha);
