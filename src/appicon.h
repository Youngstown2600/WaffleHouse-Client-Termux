#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

inline QString appLogoResourcePath()
{
    return QStringLiteral(":/branding/wafflehouse-client-logo.png");
}

inline QIcon appIcon()
{
    QIcon icon;
    icon.addFile(QStringLiteral(":/icons/16x16/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/22x22/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/24x24/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/32x32/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/48x48/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/64x64/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/128x128/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/256x256/wafflehouse-client.png"));
    icon.addFile(QStringLiteral(":/icons/512x512/wafflehouse-client.png"));
    return icon;
}

inline QPixmap appLogoPixmap()
{
    return QPixmap(appLogoResourcePath());
}
