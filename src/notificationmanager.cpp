#include "notificationmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

#ifndef WAFFLEHOUSE_SOURCE_SOUND_DIR
#define WAFFLEHOUSE_SOURCE_SOUND_DIR ""
#endif

namespace {
QString normalizedIdentity(QString value, ConnectionSettings::Protocol protocol)
{
    value = value.trimmed().toCaseFolded();
    if (protocol == ConnectionSettings::Protocol::Oscar) value.remove(QLatin1Char(' '));
    return value;
}

QString speakerFromLine(const QString &text)
{
    if (!text.startsWith(QLatin1Char('<'))) return {};
    const int end = text.indexOf(QLatin1Char('>'));
    if (end <= 1) return {};
    return text.mid(1, end - 1).trimmed();
}

QString messageFromLine(const QString &text)
{
    if (!text.startsWith(QLatin1Char('<'))) return text;
    const int end = text.indexOf(QLatin1Char('>'));
    if (end < 0) return text;
    return text.mid(end + 1).trimmed();
}

bool containsIrcMention(const QString &message, const QStringList &identities)
{
    for (const QString &candidate : identities) {
        const QString nick = candidate.trimmed();
        if (nick.isEmpty()) continue;
        const QString pattern = QStringLiteral("(^|[^A-Za-z0-9_\\-\\[\\]\\\\`^{}|])%1([^A-Za-z0-9_\\-\\[\\]\\\\`^{}|]|$)")
                                    .arg(QRegularExpression::escape(nick));
        if (QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption)
                .match(message).hasMatch()) return true;
    }
    return false;
}

void terminalBell()
{
    std::fputc('\a', stderr);
    std::fflush(stderr);
}
}

QString NotificationManager::key(Event event)
{
    switch (event) {
    case Event::IrcMention: return QStringLiteral("irc-mention");
    case Event::IrcPrivateMessage: return QStringLiteral("irc-pm");
    case Event::AimInstantMessage: return QStringLiteral("aim-im");
    case Event::AimChatMessage: return QStringLiteral("aim-chat");
    case Event::FileComplete: return QStringLiteral("file-complete");
    case Event::ConnectionWarning: return QStringLiteral("connection-warning");
    }
    return QStringLiteral("unknown");
}

std::optional<NotificationManager::Event> NotificationManager::eventFromKey(const QString &value)
{
    QString k = value.trimmed().toCaseFolded();
    k.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (k == QStringLiteral("irc-mention") || k == QStringLiteral("mention")) return Event::IrcMention;
    if (k == QStringLiteral("irc-pm") || k == QStringLiteral("irc-im") || k == QStringLiteral("pm")) return Event::IrcPrivateMessage;
    if (k == QStringLiteral("aim-im") || k == QStringLiteral("aim-pm")) return Event::AimInstantMessage;
    if (k == QStringLiteral("aim-chat") || k == QStringLiteral("chat")) return Event::AimChatMessage;
    if (k == QStringLiteral("file-complete")) return Event::FileComplete;
    if (k == QStringLiteral("connection-warning")) return Event::ConnectionWarning;
    return std::nullopt;
}

QString NotificationManager::displayName(Event event)
{
    switch (event) {
    case Event::IrcMention: return QStringLiteral("IRC channel mention");
    case Event::IrcPrivateMessage: return QStringLiteral("IRC private message");
    case Event::AimInstantMessage: return QStringLiteral("AIM instant message");
    case Event::AimChatMessage: return QStringLiteral("AIM chat message");
    case Event::FileComplete: return QStringLiteral("File transfer complete");
    case Event::ConnectionWarning: return QStringLiteral("Connection warning");
    }
    return QStringLiteral("Notification");
}

QString NotificationManager::defaultBuiltin(Event event)
{
    switch (event) {
    case Event::IrcMention: return QStringLiteral("mention.wav");
    case Event::IrcPrivateMessage: return QStringLiteral("message.wav");
    case Event::AimInstantMessage: return QStringLiteral("aim-im.wav");
    case Event::AimChatMessage: return QStringLiteral("aim-chat.wav");
    case Event::FileComplete: return QStringLiteral("complete.wav");
    case Event::ConnectionWarning: return QStringLiteral("warning.wav");
    }
    return QStringLiteral("message.wav");
}

QList<NotificationManager::Event> NotificationManager::configurableEvents()
{
    return {Event::IrcMention, Event::IrcPrivateMessage, Event::AimInstantMessage,
            Event::AimChatMessage};
}

bool NotificationManager::globalEnabled()
{
    QSettings settings;
    return settings.value(QStringLiteral("notifications/enabled"), true).toBool();
}

void NotificationManager::setGlobalEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("notifications/enabled"), enabled);
    settings.sync();
}

NotificationManager::Setting NotificationManager::setting(Event event)
{
    QSettings settings;
    const QString base = QStringLiteral("notifications/events/%1/").arg(key(event));
    Setting out;
    out.enabled = settings.value(base + QStringLiteral("enabled"), true).toBool();
    out.soundSpec = settings.value(base + QStringLiteral("sound"), builtinSpec(event)).toString();
    if (out.soundSpec.trimmed().isEmpty()) out.soundSpec = builtinSpec(event);
    return out;
}

void NotificationManager::setSetting(Event event, const Setting &value)
{
    QSettings settings;
    const QString base = QStringLiteral("notifications/events/%1/").arg(key(event));
    settings.setValue(base + QStringLiteral("enabled"), value.enabled);
    settings.setValue(base + QStringLiteral("sound"), value.soundSpec);
    settings.sync();
}

QString NotificationManager::builtinSpec(Event event)
{
    return QStringLiteral("builtin:%1").arg(defaultBuiltin(event));
}

QString NotificationManager::customSpec(const QString &path)
{
    return QStringLiteral("custom:%1").arg(QDir::cleanPath(path.trimmed()));
}

bool NotificationManager::isBuiltinSpec(const QString &spec)
{
    return spec.startsWith(QStringLiteral("builtin:"), Qt::CaseInsensitive);
}

bool NotificationManager::isCustomSpec(const QString &spec)
{
    return spec.startsWith(QStringLiteral("custom:"), Qt::CaseInsensitive);
}

QString NotificationManager::customPath(const QString &spec)
{
    return isCustomSpec(spec) ? spec.mid(QStringLiteral("custom:").size()) : QString();
}

QString NotificationManager::resolveBuiltin(const QString &name)
{
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << QDir(appDir).filePath(QStringLiteral("sounds/%1").arg(name));
    candidates << QDir(appDir).filePath(QStringLiteral("../share/wafflehouse-client/sounds/%1").arg(name));
    candidates << QDir(appDir).filePath(QStringLiteral("../share/WaffleHouseClient/sounds/%1").arg(name));
    const QString sourceDir = QString::fromUtf8(WAFFLEHOUSE_SOURCE_SOUND_DIR);
    if (!sourceDir.isEmpty()) candidates << QDir(sourceDir).filePath(name);
    candidates << QDir::current().filePath(QStringLiteral("sounds/%1").arg(name));
    for (const QString &path : candidates) {
        QFileInfo info(path);
        if (info.isFile() && info.isReadable()) return info.absoluteFilePath();
    }
    return {};
}

bool NotificationManager::launchPlayer(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return false;

#ifdef WAFFLEHOUSE_TERMUX
    const QString termuxPlayer = QStandardPaths::findExecutable(QStringLiteral("termux-media-player"));
    if (!termuxPlayer.isEmpty()) {
        if (QProcess::startDetached(termuxPlayer, {QStringLiteral("play"), path})) return true;
    }
#endif

    const QString paplay = QStandardPaths::findExecutable(QStringLiteral("paplay"));
    if (!paplay.isEmpty()) return QProcess::startDetached(paplay, {path});

    const QString pwPlay = QStandardPaths::findExecutable(QStringLiteral("pw-play"));
    if (!pwPlay.isEmpty()) return QProcess::startDetached(pwPlay, {path});

    const QString aplay = QStandardPaths::findExecutable(QStringLiteral("aplay"));
    if (!aplay.isEmpty()) return QProcess::startDetached(aplay, {QStringLiteral("-q"), path});

    const QString ffplay = QStandardPaths::findExecutable(QStringLiteral("ffplay"));
    if (!ffplay.isEmpty()) {
        return QProcess::startDetached(ffplay,
            {QStringLiteral("-nodisp"), QStringLiteral("-autoexit"),
             QStringLiteral("-loglevel"), QStringLiteral("quiet"), path});
    }
    return false;
}

bool NotificationManager::playSpec(const QString &spec, bool terminalBellFallback)
{
    const QString trimmed = spec.trimmed();
    if (trimmed.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0) return true;

    QString path;
    if (isBuiltinSpec(trimmed)) {
        path = resolveBuiltin(trimmed.mid(QStringLiteral("builtin:").size()));
    } else if (isCustomSpec(trimmed)) {
        path = customPath(trimmed);
    } else {
        // A bare path is accepted for compatibility with CLI-entered settings.
        path = trimmed;
    }

    if (launchPlayer(path)) return true;
    if (terminalBellFallback) terminalBell();
    return false;
}

bool NotificationManager::play(Event event, bool terminalBellFallback)
{
    if (!globalEnabled()) return false;
    const Setting cfg = setting(event);
    if (!cfg.enabled) return false;
    return playSpec(cfg.soundSpec, terminalBellFallback);
}

std::optional<NotificationManager::Event> NotificationManager::classifyIncoming(
    const ConnectionSettings &settings,
    const QString &identity,
    const QString &kind,
    const QString &text)
{
    if (kind != QStringLiteral("im") && kind != QStringLiteral("chat")) return std::nullopt;
    const QString speaker = speakerFromLine(text);
    if (speaker.isEmpty()) return std::nullopt;

    const QString selfIdentity = normalizedIdentity(identity, settings.protocol);
    const QString configuredIdentity = normalizedIdentity(settings.username, settings.protocol);
    const QString normalizedSpeaker = normalizedIdentity(speaker, settings.protocol);
    if ((!selfIdentity.isEmpty() && normalizedSpeaker == selfIdentity)
        || (!configuredIdentity.isEmpty() && normalizedSpeaker == configuredIdentity)) {
        return std::nullopt;
    }

    if (kind == QStringLiteral("im")) {
        if (settings.protocol == ConnectionSettings::Protocol::Irc) return Event::IrcPrivateMessage;
        if (settings.protocol == ConnectionSettings::Protocol::Oscar) return Event::AimInstantMessage;
        return std::nullopt;
    }

    if (settings.protocol == ConnectionSettings::Protocol::Oscar) {
        return Event::AimChatMessage;
    }
    if (settings.protocol == ConnectionSettings::Protocol::Irc) {
        QStringList identities;
        if (!identity.trimmed().isEmpty()) identities << identity.trimmed();
        if (!settings.username.trimmed().isEmpty()
            && !identities.contains(settings.username.trimmed(), Qt::CaseInsensitive)) {
            identities << settings.username.trimmed();
        }
        if (containsIrcMention(messageFromLine(text), identities)) return Event::IrcMention;
    }
    return std::nullopt;
}
