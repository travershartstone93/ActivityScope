#include "icons.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

QPixmap iconPixmap(const QString& name, const QColor& color, int px, qreal dpr) {
    static QHash<QString, QPixmap> cache;
    const QString key = name + color.name(QColor::HexArgb) + QString::number(px) + QString::number(dpr);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    QFile f(":/assets/icons/" + name + ".svg");
    QByteArray svg;
    if (f.open(QIODevice::ReadOnly)) svg = f.readAll();
    svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
    if (color.alpha() < 255) svg.replace("stroke-width=\"2\"", QByteArray("stroke-width=\"2\" stroke-opacity=\"") + QByteArray::number(color.alphaF(), 'f', 2) + "\"");
    QSvgRenderer r(svg);
    QPixmap pm(int(px * dpr), int(px * dpr));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p);
    p.end();
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

QIcon themedIcon(const QString& name, const QColor& color) { return QIcon(iconPixmap(name, color, 20)); }
