#pragma once

#include <QString>

#ifndef APP_DISPLAY_NAME
#define APP_DISPLAY_NAME "WaffleHouse-Termux"
#endif
#ifndef APP_ID
#define APP_ID "WaffleHouseClient"
#endif
#ifndef APP_EXECUTABLE
#define APP_EXECUTABLE "wafflehouse-termux"
#endif
#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "1.0"
#endif

inline QString appDisplayName() { return QString::fromUtf8(APP_DISPLAY_NAME); }
inline QString appId() { return QString::fromUtf8(APP_ID); }
inline QString appExecutableName() { return QString::fromUtf8(APP_EXECUTABLE); }
inline QString appVersionString() { return QString::fromUtf8(APP_VERSION_STRING); }
inline QString appEditionName() { return QStringLiteral("Termux"); }
inline QString appEditionDisplayName() { return appDisplayName() + QStringLiteral(" ") + appVersionString(); }
inline QString appDefaultRealName() { return appDisplayName() + QStringLiteral(" User"); }

inline QString appAsciiLogo()
{
    return QString::fromUtf8(
R"LOGO(▄     ▄  ▄▄▄▄▄  ▄▄▄▄▄▄ ▄▄▄▄▄▄ ▄      ▄▄▄▄▄▄ ▄     ▄  ▄▄▄▄▄▄ ▄     ▄  ▄▄▄▄▄▄ ▄▄▄▄▄▄
█     █ █     █ █      █      █      █      █     ▄ █     ▄ █     █ █       █     
█  ▄  ▄ █▄▄▄▄▄▀ █▄▄▄   █▄▄▄   █      █▄▄▄   █▀▀▀▀▀█ █     ▄ █     ▄ █▄▄▄▄▄▄ █▄▄▄  
▄  ▀  █ █     ▀ ▄      ▄      █      ▄      █     ▀ █     █ ▄     █       ▀ ▄     
▀▄▀ ▀▄▀ █     █ █      █      █▄▄▄▄▄ █▄▄▄▄▄ █     █ ▀▄▄▄▄▄▀ ▀▄▄▄▄▄▀ ▄▄▄▄▄▄▀ █▄▄▄▄▄)LOGO");
}
