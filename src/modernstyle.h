#pragma once

#include <QString>

namespace ModernStyle {

struct Palette {
    QString background;
    QString sidebar;
    QString surface;
    QString surfaceRaised;
    QString text;
    QString muted;
    QString accent;
    QString accentAlt;
    QString border;
    QString success;
    QString danger;
    bool light = false;
};

QString normalizeThemeKey(const QString &key);
Palette paletteFor(const QString &key);
QString styleSheet(const QString &key);

} // namespace ModernStyle
