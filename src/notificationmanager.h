#pragma once

#include "backend.h"

#include <QString>
#include <QStringList>
#include <optional>

class NotificationManager {
public:
    enum class Event {
        IrcMention,
        IrcPrivateMessage,
        AimInstantMessage,
        AimChatMessage,
        FileComplete,
        ConnectionWarning
    };

    struct Setting {
        bool enabled = true;
        QString soundSpec;
    };

    static QString key(Event event);
    static QString displayName(Event event);
    static std::optional<Event> eventFromKey(const QString &key);
    static QString defaultBuiltin(Event event);
    static QList<Event> configurableEvents();

    static bool globalEnabled();
    static void setGlobalEnabled(bool enabled);
    static Setting setting(Event event);
    static void setSetting(Event event, const Setting &setting);

    static QString builtinSpec(Event event);
    static QString customSpec(const QString &path);
    static bool isBuiltinSpec(const QString &spec);
    static bool isCustomSpec(const QString &spec);
    static QString customPath(const QString &spec);

    static bool play(Event event, bool terminalBellFallback = true);
    static bool playSpec(const QString &spec, bool terminalBellFallback = true);

    // Returns a notification event only for an incoming user message. Outgoing
    // echoes, status lines and IRC channel chatter without a mention are ignored.
    static std::optional<Event> classifyIncoming(const ConnectionSettings &settings,
                                                 const QString &identity,
                                                 const QString &kind,
                                                 const QString &text);

private:
    static QString resolveBuiltin(const QString &name);
    static bool launchPlayer(const QString &path);
};
