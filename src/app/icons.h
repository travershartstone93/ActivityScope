#pragma once
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

// Lucide icons are stroke drawings in "currentColor"; this paints them in any colour.
QPixmap iconPixmap(const QString& name, const QColor& color, int px, qreal dpr = 2.0);
QIcon themedIcon(const QString& name, const QColor& color);
