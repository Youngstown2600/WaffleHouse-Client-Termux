#pragma once

#include "backend.h"
#include <QStringList>

#ifndef WAFFLEHOUSE_FEATURE_OSCAR
#define WAFFLEHOUSE_FEATURE_OSCAR 1
#endif
#ifndef WAFFLEHOUSE_FEATURE_IRC
#define WAFFLEHOUSE_FEATURE_IRC 1
#endif
#ifndef WAFFLEHOUSE_FEATURE_TELNET
#define WAFFLEHOUSE_FEATURE_TELNET 1
#endif
#ifndef WAFFLEHOUSE_FEATURE_SIP
#define WAFFLEHOUSE_FEATURE_SIP 1
#endif
#ifndef WAFFLEHOUSE_FEATURE_MEDIA
#define WAFFLEHOUSE_FEATURE_MEDIA 1
#endif

namespace BuildFeatures {
inline constexpr bool Oscar = WAFFLEHOUSE_FEATURE_OSCAR != 0;
inline constexpr bool Irc = WAFFLEHOUSE_FEATURE_IRC != 0;
inline constexpr bool Telnet = WAFFLEHOUSE_FEATURE_TELNET != 0;
inline constexpr bool Sip = WAFFLEHOUSE_FEATURE_SIP != 0;
inline constexpr bool Media = WAFFLEHOUSE_FEATURE_MEDIA != 0;

inline bool protocolEnabled(ConnectionSettings::Protocol protocol)
{
    switch (protocol) {
    case ConnectionSettings::Protocol::Oscar: return Oscar;
    case ConnectionSettings::Protocol::Irc: return Irc;
    case ConnectionSettings::Protocol::Telnet: return Telnet;
    case ConnectionSettings::Protocol::Sip: return Sip;
    case ConnectionSettings::Protocol::Unknown: return false;
    }
    return false;
}

inline QStringList enabledProtocolNames(bool includeMedia = true)
{
    QStringList out;
    if (Oscar) out << QStringLiteral("AIM/OSCAR");
    if (Irc) out << QStringLiteral("IRC");
    if (Telnet) out << QStringLiteral("TELNET/BBS");
    if (Sip) out << QStringLiteral("SIP/VOIP");
    if (includeMedia && Media) out << QStringLiteral("MEDIA/RADIO");
    return out;
}
}
